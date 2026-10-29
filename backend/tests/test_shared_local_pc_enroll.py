"""S4D local-only owner PC enrollment, real HTTP and private token file.

Temporary 0700 state and secret directories only. Never touches running
ORUN Backend, PC Edge, Android, Gateway or live production credentials.
"""
from __future__ import annotations

import http.client
from http.server import ThreadingHTTPServer
import json
import os
from pathlib import Path
import stat
import subprocess
import sys
import tempfile
import threading
import unittest

from backend.edge_enrollment import EdgeEnrollment
from backend.shared_sync_http import handler_factory
from backend.shared_sync_store import SharedStore, Actor
from tools.orun_shared_sync_loopback import prepare, FILES
from tools.orun_shared_local_pc_enroll import (
    authorize_local_pc, LocalPcEnrollError, READ_ONLY,
)
from tools.orun_shared_pc_mirror import PcSharedMirror, run_once

ZONE = {"name": "Kuzey", "active": True, "ring_e7": [
    [376100000, 280540000], [376100000, 280550000],
    [376110000, 280550000], [376110000, 280540000]]}


class LocalPcEnrollmentTests(unittest.TestCase):
    def setUp(self):
        self.t = tempfile.TemporaryDirectory(prefix="orun-s4d-local-")
        self.addCleanup(self.t.cleanup)
        self.root = Path(self.t.name)
        self.state = self.root / "backend"
        self.secrets = self.root / "private-tokens"
        self.state.mkdir(mode=0o700)
        self.secrets.mkdir(mode=0o700)
        self.store, self.registry = prepare(self.state)
        self.repo = Path(__file__).resolve().parents[2]
        self.service = ThreadingHTTPServer(("127.0.0.1", 0),
            handler_factory(self.store, self.registry.resolve_headers))
        self.service.daemon_threads = True
        worker = threading.Thread(target=self.service.serve_forever, daemon=True)
        worker.start()
        self.addCleanup(worker.join, 5)
        self.addCleanup(self.service.server_close)
        self.addCleanup(self.service.shutdown)
        self.origin = f"http://127.0.0.1:{self.service.server_port}"

    def cli(self, installation="pc-01", token_out=None, approve=True, **kwargs):
        path = token_out or self.secrets / "pc-01.token"
        args = [sys.executable, "tools/orun_shared_local_pc_enroll.py",
                "--state-dir", str(self.state), "--site", kwargs.get("site", "site-a"),
                "--actor", kwargs.get("actor", "owner"),
                "--installation", installation,
                "--token-out", str(path)]
        if approve:
            args.append("--authorize-local-pc")
        return subprocess.run(args, cwd=self.repo, capture_output=True,
                              text=True, timeout=15)

    def get(self, path, bearer=None):
        conn = http.client.HTTPConnection("127.0.0.1", self.service.server_port, timeout=5)
        try:
            headers = {"Authorization": "Bearer " + bearer} if bearer else {}
            conn.request("GET", path, headers=headers)
            result = conn.getresponse()
            return result.status, json.loads(result.read())
        finally:
            conn.close()

    def post(self, path, bearer, data):
        conn = http.client.HTTPConnection("127.0.0.1", self.service.server_port, timeout=5)
        try:
            conn.request("POST", path, json.dumps(data), headers={
                "Authorization": "Bearer " + bearer, "Content-Type": "application/json"})
            result = conn.getresponse()
            return result.status, json.loads(result.read())
        finally:
            conn.close()

    def test_operator_opt_in_absent_does_not_create_any_auth_or_token(self):
        denied = self.cli(approve=False)
        self.assertEqual(denied.returncode, 2)
        self.assertIn("LOCAL_OWNER_APPROVAL_REQUIRED", denied.stderr)
        self.assertFalse((self.secrets / "pc-01.token").exists())
        with self.registry._session() as db:
            self.assertEqual(db.execute("SELECT COUNT(*) FROM edge_installs").fetchone()[0], 0)
            self.assertEqual(db.execute("SELECT COUNT(*) FROM edge_pending").fetchone()[0], 0)

    def test_actual_cli_approval_readonly_real_http_and_private_pc_mirror(self):
        status = self.cli()
        self.assertEqual(status.returncode, 0, status.stderr)
        self.assertIn("ORUN_LOCAL_PC_ENROLLED_READ_ONLY=YES", status.stdout)
        private = self.secrets / "pc-01.token"
        self.assertEqual(stat.S_IMODE(private.stat().st_mode), 0o600)
        bearer = private.read_text().strip()
        self.assertGreaterEqual(len(bearer), 48)
        self.assertNotIn(bearer, status.stdout + status.stderr)
        identity_code, identity = self.get("/api/v1/sync/identity", bearer)
        self.assertEqual(identity_code, 200)
        self.assertEqual(identity, {"site": "site-a", "installation": "pc-01"})
        actor = self.registry.resolve_token(bearer)
        self.assertEqual(actor, Actor("site-a", "owner", "pc-01", READ_ONLY))
        answer_code, response = self.post("/api/v1/sync/ops", bearer, {"ops":[{
            "op_id": "denied-write", "kind": "zone", "id": "z1",
            "action": "UPSERT", "base_revision": 0, "body": ZONE
        }]})
        self.assertEqual(answer_code, 200)
        self.assertEqual(response["results"][0]["status"], "REJECTED")
        # Test setup creates one trusted site record. The PC can READ it but
        # did not gain any right to WRITE it via local enrollment.
        admin = Actor("site-a", "server-admin", "trusted-seed", frozenset({"zone:write"}))
        self.assertEqual(self.store.mutate(admin, [{
            "op_id": "seed-1", "kind": "zone", "id": "z1",
            "action": "UPSERT", "base_revision": 0, "body": ZONE
        }])[0]["status"], "ACCEPTED")
        mirror = PcSharedMirror(self.root / "private-mirror.sqlite",
                                "site-a", "pc-01", self.origin)
        self.assertEqual(run_once(mirror, bearer)["received"], 1)
        self.assertEqual(mirror.snapshot()["resources"][0]["body"], ZONE)
        # Independently launched PC mirror CLI uses the owner-only token file.
        cmd = [sys.executable, "tools/orun_shared_pc_mirror.py",
               "--site", "site-a", "--installation", "pc-01",
               "--db", str(self.root / "private-mirror.sqlite"),
               "--url", self.origin, "--token-file", str(private), "--once"]
        another = subprocess.run(cmd, cwd=self.repo, capture_output=True,
                                 text=True, timeout=15)
        self.assertEqual(another.returncode, 0, another.stderr)
        self.assertEqual(json.loads(another.stdout)["received"], 0)
        self.assertNotIn(bearer, another.stdout + another.stderr)

    def test_second_installation_is_distinct_revocation_does_not_erase_mirror(self):
        self.assertEqual(self.cli().returncode, 0)
        otherfile = self.secrets / "pc-02.token"
        self.assertEqual(self.cli("pc-02", token_out=otherfile).returncode, 0)
        first = (self.secrets / "pc-01.token").read_text().strip()
        other = otherfile.read_text().strip()
        self.assertNotEqual(first, other)
        self.assertTrue(self.registry.revoke("pc-01"))
        self.assertEqual(self.get("/api/v1/sync", first)[0], 401)
        self.assertEqual(self.get("/api/v1/sync/identity", other)[0], 200)
        again = self.cli()
        self.assertNotEqual(again.returncode, 0)
        self.assertEqual((self.secrets / "pc-01.token").read_text().strip(), first)

    def test_invalid_paths_ids_or_existing_credential_refused(self):
        existing = self.secrets / "keep.token"
        existing.write_text("DO-NOT-OVERWRITE")
        os.chmod(existing, 0o600)
        with self.assertRaisesRegex(LocalPcEnrollError, "TOKEN_OUTPUT_MUST_BE_NEW_ABSOLUTE_FILE"):
            authorize_local_pc(self.state, site="site-a", actor="owner",
                               installation="pc-01", token_out=existing,
                               explicitly_approved=True)
        self.assertEqual(existing.read_text(), "DO-NOT-OVERWRITE")
        for installation in ("phone-a", "../pc-a", "pc-evil;", "pc "):
            with self.subTest(installation=installation):
                result = self.cli(installation, token_out=self.secrets / "different.token")
                self.assertEqual(result.returncode, 2)
        os.chmod(self.secrets, 0o755)
        result = self.cli()
        self.assertEqual(result.returncode, 2)
        self.assertIn("TOKEN_PARENT_MUST_BE_OWNER_ONLY", result.stderr)
        self.assertFalse((self.secrets / "pc-01.token").exists())


if __name__ == "__main__":
    unittest.main(verbosity=2)
