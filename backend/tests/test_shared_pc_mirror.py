"""S4A live HTTP tests: PC read mirror never touches running PC Edge.

Temporary Step3C enrollment DB, SharedStore and per-installation private
mirror; IPv4 127.0.0.1 only. Test read cursors, site/installation isolation,
denial, restart, transactional rollback and fail-closed corruption.
"""
from __future__ import annotations

import http.server
from pathlib import Path
import os
import stat
import tempfile
import threading
import unittest

from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
from cryptography.hazmat.primitives.serialization import Encoding, PublicFormat

from backend.edge_enrollment import EdgeEnrollment, signed_challenge
from backend.shared_sync_http import handler_factory
from backend.shared_sync_store import SharedStore
from tools.orun_shared_pc_mirror import (
    PcSharedMirror, MirrorError, run_once, validate_origin,
)

ZONE = {"name": "Kuzey", "active": True, "ring_e7": [
    [376100000, 280540000], [376100000, 280550000],
    [376110000, 280550000], [376110000, 280540000]]}
GROUP = {"name": "Sürü 1", "device_ids": ["T-01", "T-02"]}
TARGET = {"zone_id": "z1", "scope": "selected",
          "device_ids": ["T-01"], "group_ids": ["g1"]}
READ = frozenset({"zone:read", "group:read", "zone_target:read"})
WRITE = READ | frozenset({"zone:write", "group:write", "zone_target:write"})


def enrollment(reg, installation, site, actor, scopes):
    key = Ed25519PrivateKey.generate()
    public = key.public_key().public_bytes(Encoding.Raw, PublicFormat.Raw)
    pending = reg.begin(installation, public)
    code = reg.approve(pending.request_id, pending.fingerprint, site, actor, scopes)
    return reg.finish(pending.request_id, installation, public,
                      key.sign(signed_challenge(pending, installation, public)), code)


def op(opid, kind, rid, body, rev=0):
    return {"op_id": opid, "kind": kind, "id": rid,
            "action": "UPSERT", "base_revision": rev, "body": body}


class SharedPcMirrorTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="orun-pc-mirror-")
        self.addCleanup(self.temp.cleanup)
        self.dir = Path(self.temp.name)
        self.reg = EdgeEnrollment(self.dir / "enroll.sqlite")
        self.backend = SharedStore(self.dir / "backend.sqlite")
        self.tokens = {
            "author": enrollment(self.reg, "phone-author", "site-A", "author", WRITE),
            "pc": enrollment(self.reg, "pc-edge", "site-A", "viewer", READ),
            "other": enrollment(self.reg, "pc-other", "site-B", "other", READ),
            "zone": enrollment(self.reg, "zone-only", "site-A", "viewer", frozenset({"zone:read"})),
        }
        self.server = http.server.ThreadingHTTPServer(("127.0.0.1", 0),
            handler_factory(self.backend, self.reg.resolve_headers))
        self.server.daemon_threads = True
        thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        thread.start()
        self.addCleanup(thread.join, 5)
        self.addCleanup(self.server.server_close)
        self.addCleanup(self.server.shutdown)
        self.origin = "http://127.0.0.1:" + str(self.server.server_address[1])
        self.store_path = self.dir / "pc_mirror.sqlite"

    def make(self, installation="pc-edge", site="site-A", path=None, origin=None):
        return PcSharedMirror(path or self.store_path, site, installation,
                              origin or self.origin)

    def publish(self):
        actor = self.reg.resolve_token(self.tokens["author"])
        results = self.backend.mutate(actor, [
            op("o1", "zone", "z1", ZONE),
            op("o2", "group", "g1", GROUP),
            op("o3", "zone_target", "z1", TARGET),
        ])
        self.assertEqual([r["status"] for r in results], ["ACCEPTED"] * 3)

    def test_two_client_convergence_restart_and_revision(self):
        self.publish()
        pc = self.make()
        result = run_once(pc, self.tokens["pc"])
        self.assertEqual(result["received"], 3)
        self.assertEqual(result["cursor"], 3)
        snapshot = pc.snapshot()
        self.assertEqual(len(snapshot["resources"]), 3)
        self.assertEqual(next(r for r in snapshot["resources"] if
                              r["kind"] == "zone")["body"], ZONE)
        self.assertEqual(run_once(self.make(), self.tokens["pc"])["received"], 0)
        next_zone = {**ZONE, "name": "Güney"}
        actor = self.reg.resolve_token(self.tokens["author"])
        self.assertEqual(self.backend.mutate(actor, [
            op("o4", "zone", "z1", next_zone, 1)])[0]["status"], "ACCEPTED")
        self.assertEqual(run_once(self.make(), self.tokens["pc"])["received"], 1)
        zone = next(r for r in self.make().snapshot()["resources"] if r["kind"] == "zone")
        self.assertEqual(zone["revision"], 2)
        self.assertEqual(zone["body"]["name"], "Güney")
        self.assertEqual(stat.S_IMODE(self.store_path.stat().st_mode), 0o600)
        self.assertEqual(stat.S_IMODE(self.store_path.with_name(
            self.store_path.name + ".lock").stat().st_mode), 0o600)

    def test_mismatched_enrolled_token_rejected_before_resource_get(self):
        self.publish()
        pc = self.make()
        with self.assertRaisesRegex(MirrorError, "SYNC_SERVER_IDENTITY_MISMATCH"):
            run_once(pc, self.tokens["other"])
        self.assertEqual(pc.snapshot()["cursor"], 0)
        self.assertEqual(pc.snapshot()["resources"], [])
        with self.assertRaisesRegex(MirrorError, "SYNC_SERVER_IDENTITY_MISMATCH"):
            run_once(pc, self.tokens["zone"])
        self.assertEqual(pc.snapshot()["cursor"], 0)
        self.assertEqual(run_once(pc, self.tokens["pc"])["received"], 3)

    def test_revocation_is_not_a_mirror_reset(self):
        self.publish()
        pc = self.make()
        run_once(pc, self.tokens["pc"])
        original = pc.snapshot()
        self.assertTrue(self.reg.revoke("pc-edge"))
        with self.assertRaisesRegex(MirrorError, "SYNC_AUTH_EXPIRED"):
            run_once(pc, self.tokens["pc"])
        self.assertEqual(pc.snapshot(), original)

    def test_one_kind_scope_cannot_pull_other_resources(self):
        self.publish()
        only = self.make("zone-only", path=self.dir / "zone.sqlite")
        self.assertEqual(run_once(only, self.tokens["zone"])["received"], 1)
        self.assertEqual({r["kind"] for r in only.snapshot()["resources"]}, {"zone"})

    def test_bad_page_and_late_conflict_rollback_whole_transaction(self):
        pc = self.make()
        first = {"changes": [
            {"sequence": 2, "kind": "zone", "id": "z1", "revision": 2,
             "deleted": False, "body": ZONE}],
            "cursor": 2, "has_more": False}
        pc.apply_page(first, 0)
        original = pc.snapshot()
        corrupted = {"changes": [
            {"sequence": 3, "kind": "group", "id": "g1", "revision": 1,
             "deleted": False, "body": GROUP},
            {"sequence": 4, "kind": "zone", "id": "z1", "revision": 1,
             "deleted": False, "body": {**ZONE, "name": "Stale"}}],
            "cursor": 4, "has_more": False}
        with self.assertRaisesRegex(MirrorError, "SYNC_REVISION_CONFLICT"):
            pc.apply_page(corrupted, 2)
        self.assertEqual(pc.snapshot(), original)
        with self.assertRaisesRegex(MirrorError, "SYNC_CURSOR_INVALID"):
            pc.apply_page({"changes": [], "cursor": 99, "has_more": False}, 2)
        self.assertEqual(pc.snapshot(), original)

    def test_local_binding_corruption_and_origin_rejections(self):
        self.make()
        with self.assertRaisesRegex(MirrorError, "SYNC_MIRROR_BOUND_TO_ANOTHER_ACCOUNT"):
            self.make(site="site-B")
        with self.assertRaisesRegex(MirrorError, "SYNC_MIRROR_BOUND_TO_ANOTHER_ACCOUNT"):
            self.make(installation="another-pc")
        import sqlite3
        with sqlite3.connect(self.store_path) as db:
            db.execute("INSERT INTO mirror_resources VALUES(?,?,?,?,?,?)",
                       ("zone", "z1", 1, 0, "not JSON", 1))
        with self.assertRaisesRegex(MirrorError, "SYNC_MIRROR_CORRUPT"):
            self.make()
        for address in ["http://100.101.102.103:8090", "http://localhost:8090",
                        "http://evil.example", "https://user:pass@example.com",
                        "https://example.com/sync", "https://example.com/?token=abc",
                        "ftp://example.com"]:
            with self.subTest(address=address):
                with self.assertRaises(MirrorError):
                    validate_origin(address)
        self.assertEqual(validate_origin(self.origin), self.origin)

    def test_reject_redirect_and_unknown_token_without_fetch(self):
        class Redirect(http.server.BaseHTTPRequestHandler):
            def do_GET(self):
                self.send_response(302)
                self.send_header("Location", "https://example.com/redirect")
                self.end_headers()
            def log_message(self, *args):
                pass
        server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Redirect)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            redir = self.make(path=self.dir / "redirect.sqlite",
                              origin="http://127.0.0.1:" + str(server.server_address[1]))
            with self.assertRaisesRegex(MirrorError, "SYNC_HTTP_FAILED"):
                run_once(redir, self.tokens["pc"])
            self.assertEqual(redir.snapshot()["cursor"], 0)
        finally:
            server.shutdown()
            server.server_close()
            thread.join(timeout=5)
        with self.assertRaisesRegex(MirrorError, "SYNC_ENROLLMENT_REQUIRED"):
            run_once(self.make(), "shared-bench-token")


if __name__ == "__main__":
    unittest.main(verbosity=2)
