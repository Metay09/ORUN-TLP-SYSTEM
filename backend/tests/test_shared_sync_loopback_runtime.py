"""S4C genuine subprocess backend runtime: isolated HTTP + private SQLite.

No ORUN production service, USB, BLE, Gateway, firmware or Android change.
"""
from __future__ import annotations

import http.client
import json
import os
from pathlib import Path
import select
import stat
import subprocess
import sys
import tempfile
import unittest

from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
from cryptography.hazmat.primitives.serialization import Encoding, PublicFormat

from backend.edge_enrollment import EdgeEnrollment, signed_challenge
from backend.shared_sync_store import SharedStore
from tools.orun_shared_pc_mirror import PcSharedMirror, run_once
from tools.orun_shared_sync_loopback import (
    FILES, StartupRefused, prepare, start,
)

READ = frozenset({"zone:read", "group:read", "zone_target:read"})
EDIT = READ | frozenset({"zone:write", "group:write", "zone_target:write"})
RING = [[376100000, 280540000], [376100000, 280550000],
        [376110000, 280550000], [376110000, 280540000]]
ZONE = {"name": "Kuzey", "active": True, "ring_e7": RING}


def enroll(reg, installation, site, actor, scopes):
    key = Ed25519PrivateKey.generate()
    pub = key.public_key().public_bytes(Encoding.Raw, PublicFormat.Raw)
    pending = reg.begin(installation, pub)
    code = reg.approve(pending.request_id, pending.fingerprint, site, actor, scopes)
    sig = key.sign(signed_challenge(pending, installation, pub))
    return reg.finish(pending.request_id, installation, pub, sig, code)


def get(port, path, token=None, spoof=None):
    c = http.client.HTTPConnection("127.0.0.1", port, timeout=5)
    headers = dict(spoof or {})
    if token:
        headers["Authorization"] = "Bearer " + token
    try:
        c.request("GET", path, headers=headers)
        r = c.getresponse()
        return r.status, json.loads(r.read())
    finally:
        c.close()


class LoopbackRuntimeTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="orun-s4c-")
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.state = self.root / "shared-only"
        self.state.mkdir(mode=0o700)
        self.repo = Path(__file__).resolve().parents[2]

    def launch(self, reuse=False):
        args = [sys.executable, "-u", "tools/orun_shared_sync_loopback.py",
                "--state-dir", str(self.state), "--port", "0"]
        if reuse:
            args.append("--reuse-state")
        p = subprocess.Popen(args, cwd=self.repo, text=True,
                             stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        try:
            assert p.stdout is not None
            ready, _, _ = select.select([p.stdout], [], [], 15)
            if not ready:
                raise RuntimeError("loopback start did not announce bind")
            announced = p.stdout.readline().strip()
            if not announced.startswith("ORUN_SHARED_BIND=127.0.0.1:"):
                raise AssertionError(f"unexpected start: {announced!r}")
            port = int(announced.rsplit(":", 1)[1])
            self.assertGreater(port, 0)
            second = p.stdout.readline().strip()
            self.assertEqual(second,
                             "ORUN_SHARED_SECURITY=STEP3C_ENROLLED_ONLY;LOOPBACK_HTTP_ONLY")
        except BaseException:
            p.kill()
            p.communicate(timeout=10)
            raise
        return p, port

    @staticmethod
    def shutdown(p):
        p.terminate()
        try:
            p.communicate(timeout=8)
        except subprocess.TimeoutExpired:
            p.kill()
            p.communicate(timeout=8)
            raise
        if p.returncode not in (-15, 0):
            raise AssertionError(f"server exited unexpectedly: {p.returncode}")

    def test_separate_process_auth_sqlite_pc_mirror_restart_revocation(self):
        server, port = self.launch()
        try:
            for name in FILES:
                path = self.state / name
                self.assertTrue(path.is_file())
                self.assertEqual(stat.S_IMODE(path.stat().st_mode), 0o600)
            self.assertEqual(get(port, "/api/v1/sync")[0], 401)
            self.assertEqual(get(port, "/api/v1/sync", spoof={
                "X-Site-ID": "site-a", "X-Actor": "owner",
                "X-Scopes": "zone:read"})[0], 401)
            self.assertEqual(get(port, "/api/v1/enroll")[0], 404,
                             "no public owner approval endpoint may appear")
            reg = EdgeEnrollment(self.state / FILES[1])
            author = enroll(reg, "phone-author", "site-a", "owner", EDIT)
            pc = enroll(reg, "pc-edge", "site-a", "viewer", READ)
            other = enroll(reg, "pc-other", "site-b", "viewer", READ)
            store = SharedStore(self.state / FILES[0])
            actor = reg.resolve_token(author)
            self.assertIsNotNone(actor)
            ack = store.mutate(actor, [{
                "op_id": "real-zone", "kind": "zone", "id": "z1",
                "action": "UPSERT", "base_revision": 0, "body": ZONE}])
            self.assertEqual(ack[0]["status"], "ACCEPTED")
            self.assertEqual(get(port, "/api/v1/sync?cursor=0", pc)[0], 200)
            self.assertEqual(len(get(port, "/api/v1/sync?cursor=0", pc)[1]["changes"]), 1)
            self.assertEqual(get(port, "/api/v1/sync?cursor=0", other)[1]["changes"], [])
            mirror = PcSharedMirror(self.root / "pc.sqlite", "site-a", "pc-edge",
                                    f"http://127.0.0.1:{port}")
            self.assertEqual(run_once(mirror, pc)["received"], 1)
            self.assertEqual(mirror.snapshot()["resources"][0]["body"], ZONE)
        finally:
            self.shutdown(server)

        # New Python process, same owner-only DB pair, same bearer and
        # canonical site cursor. Neither registration nor resources vanish.
        reopened, new_port = self.launch(reuse=True)
        try:
            self.assertEqual(get(new_port, "/api/v1/sync?cursor=0", pc)[0], 200)
            self.assertEqual(len(get(new_port, "/api/v1/sync?cursor=0", pc)[1]["changes"]), 1)
            reg = EdgeEnrollment(self.state / FILES[1])
            self.assertTrue(reg.revoke("pc-edge"))
            self.assertEqual(get(new_port, "/api/v1/sync", pc)[0], 401)
            self.assertEqual(get(new_port, "/api/v1/sync", author)[0], 200)
            self.assertEqual(get(new_port, "/api/v1/sync", other)[0], 200)
        finally:
            self.shutdown(reopened)

    def test_public_bind_and_world_accessible_state_denied(self):
        with self.assertRaisesRegex(StartupRefused, "ONLY_127_0_0_1_BIND_ALLOWED"):
            start(self.state, "0.0.0.0", 0)
        self.assertFalse(any(self.state.iterdir()))
        os.chmod(self.state, 0o755)
        with self.assertRaisesRegex(StartupRefused, "PRIVATE_STATE_DIRECTORY_PERMISSIONS"):
            prepare(self.state)
        self.assertFalse(any(self.state.iterdir()))

    def test_preexisting_partial_pair_and_reuse_requires_opt_in(self):
        (self.state / FILES[0]).write_bytes(b"not a database")
        os.chmod(self.state / FILES[0], 0o600)
        with self.assertRaisesRegex(StartupRefused, "PARTIAL_STATE_PAIR_REFUSED"):
            prepare(self.state)
        with self.assertRaisesRegex(StartupRefused, "PARTIAL_STATE_PAIR_REFUSED"):
            prepare(self.state, reuse_state=True)
        self.assertFalse((self.state / FILES[1]).exists())

    def test_symlink_state_and_foreign_db_not_adopted(self):
        link = self.root / "link"
        link.symlink_to(self.state)
        with self.assertRaisesRegex(StartupRefused, "PRIVATE_STATE_DIRECTORY_REQUIRED"):
            prepare(link)
        for name in FILES:
            path = self.state / name
            path.write_bytes(b"foreign")
            os.chmod(path, 0o644)
        with self.assertRaisesRegex(StartupRefused, "STATE_MODE_MISMATCH"):
            prepare(self.state)
        with self.assertRaisesRegex(StartupRefused, "PRIVATE_DB_PERMISSIONS"):
            prepare(self.state, reuse_state=True)


if __name__ == "__main__":
    unittest.main(verbosity=2)
