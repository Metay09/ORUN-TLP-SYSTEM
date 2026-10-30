"""S6I standalone source bundle acceptance without real runtime access.

All state is synthetic TemporaryDirectory data; only random 127.0.0.1 ports.
No systemd, Tailscale, existing files, real credentials or APK are touched.
"""
from __future__ import annotations

import hashlib
import http.client
import json
import os
from pathlib import Path
import select
import sqlite3
import stat
import subprocess
import sys
import tempfile
import unittest

from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
from cryptography.hazmat.primitives.serialization import Encoding, PublicFormat

from backend.account_login import AccountStore
from backend.edge_enrollment import EdgeEnrollment, signed_challenge
from backend.shared_sync_store import Actor, SharedStore
from tools.ci.stage_account_runtime_source_s6i import (
    ROOT, SOURCES, StagingRefused, stage,
)

SITE = "orun-main"
USER = "ownerlogin"
PASSWORD = "S6I-Synthetic-Password-Only-For-Isolated-Tests!"
PC_ID = "pc-shadow-s6i"
READ = frozenset({"zone:read", "group:read", "zone_target:read"})
EDIT = frozenset({"zone:read", "zone:write", "group:read", "group:write",
                  "zone_target:read", "zone_target:write"})
RING = [[376100000, 280500000], [376100000, 280510000],
        [376110000, 280510000], [376110000, 280500000]]


def request(port: int, method: str, path: str,
            token: str | None = None, body: dict | None = None) -> tuple[int, dict]:
    conn = http.client.HTTPConnection("127.0.0.1", port, timeout=3)
    headers = {}
    if token is not None:
        headers["Authorization"] = "Bearer " + token
    payload = None
    if body is not None:
        payload = json.dumps(body).encode("utf-8")
        headers["Content-Type"] = "application/json"
    try:
        conn.request(method, path, body=payload, headers=headers)
        answer = conn.getresponse()
        return answer.status, json.loads(answer.read())
    finally:
        conn.close()


class S6ISourceStageTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="orun-s6i-isolated-")
        self.addCleanup(self.temp.cleanup)
        self.base = Path(self.temp.name)
        self.dest = self.base / "candidate"

    def test_exclusive_private_package_has_complete_pinned_closure(self):
        manifest = stage(self.dest)
        self.assertEqual(manifest["state"], "SOURCE_ONLY_NOT_INSTALLED")
        self.assertFalse(manifest["contains_private_data"])
        self.assertEqual(manifest["account_mode"], "NOT_ENABLED")
        self.assertEqual(len(manifest["sha256"]), len(SOURCES))
        self.assertEqual(stat.S_IMODE(self.dest.stat().st_mode), 0o700)
        for rel, checksum in manifest["sha256"].items():
            p = self.dest / "runtime" / rel
            self.assertEqual(hashlib.sha256(p.read_bytes()).hexdigest(), checksum)
            self.assertEqual(stat.S_IMODE(p.stat().st_mode), 0o600)
        self.assertFalse((self.dest / "state").exists())
        self.assertFalse((self.dest / "units").exists())
        with self.assertRaises(StagingRefused):
            stage(self.dest)
        self.assertEqual(
            json.loads((self.dest / "SOURCE_MANIFEST.json").read_text()),
            manifest,
        )

    def test_does_not_overwrite_live_or_follow_symlink(self):
        with self.assertRaises(StagingRefused):
            stage(Path.home() / "ORUN-SHARED-RUNTIME" / "replacement")
        with self.assertRaises(StagingRefused):
            stage(Path.home() / ".config" / "systemd" / "unit")
        alias = self.base / "alias"
        alias.symlink_to(Path.home() / "ORUN-SHARED-RUNTIME")
        with self.assertRaises(StagingRefused):
            stage(alias / "new-package")

    def _state(self) -> tuple[Path, str]:
        state = self.base / "synthetic-backend"
        state.mkdir(mode=0o700)
        (state / "shared_resources.sqlite").touch(mode=0o600)
        (state / "enrolled_edges.sqlite").touch(mode=0o600)
        registry = EdgeEnrollment(state / "enrolled_edges.sqlite")
        store = SharedStore(state / "shared_resources.sqlite")
        private = Ed25519PrivateKey.generate()
        public = private.public_key().public_bytes(
            Encoding.Raw, PublicFormat.Raw)
        pending = registry.begin(PC_ID, public)
        code = registry.approve(
            pending.request_id, pending.fingerprint, SITE, "owner", READ)
        token = registry.finish(
            pending.request_id, PC_ID, public,
            private.sign(signed_challenge(pending, PC_ID, public)),
            code,
        )
        seeded = store.mutate(
            Actor(SITE, "owner", "fixture", EDIT),
            [{"op_id": "s6i-one", "kind": "zone", "id": "example",
              "action": "UPSERT", "base_revision": 0,
              "body": {"name": "Bagimsiz mera", "active": True,
                       "ring_e7": RING}}],
        )
        self.assertEqual(seeded[0]["status"], "ACCEPTED")
        return state, token

    def _start_server(self, state: Path, *flags: str):
        # Isolated interpreter: no PYTHONPATH or project working directory
        # can accidentally satisfy a missing dependency in staged snapshot.
        launch = [
            sys.executable, "-I", "-u",
            str(self.dest / "runtime" / "tools" / "orun_shared_sync_loopback.py"),
            "--state-dir", str(state), "--bind", "127.0.0.1",
            "--port", "0", "--reuse-state", *flags,
        ]
        environment = dict(os.environ)
        environment.pop("PYTHONPATH", None)
        process = subprocess.Popen(
            launch, cwd=self.dest / "runtime", env=environment,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            text=True, bufsize=1,
        )
        self.addCleanup(self._stop, process)
        assert process.stdout is not None
        # select() bounds failure/hang; no credential or command arguments logged.
        readable, _, _ = select.select([process.stdout], [], [], 7)
        if not readable:
            raise AssertionError("STAGED_RUNTIME_START_TIMEOUT")
        first = process.stdout.readline().strip()
        if not first.startswith("ORUN_SHARED_BIND=127.0.0.1:"):
            raise AssertionError("STAGED_RUNTIME_START_REJECTED")
        port = int(first.rsplit(":", 1)[1])
        self.assertGreater(port, 0)
        return process, port

    @staticmethod
    def _stop(process: subprocess.Popen) -> None:
        if process.poll() is None:
            process.terminate()
        try:
            process.communicate(timeout=4)
        except subprocess.TimeoutExpired:
            process.kill()
            process.communicate(timeout=4)

    def test_staged_code_starts_in_default_and_explicit_account_modes(self):
        stage(self.dest)
        state, pc_token = self._state()
        process, port = self._start_server(state)
        self.assertEqual(
            request(port, "GET", "/api/v1/sync/identity", pc_token),
            (200, {"site": SITE, "installation": PC_ID}),
        )
        self.assertEqual(request(
            port, "POST", "/api/v1/account/login", pc_token,
            {"username": USER, "password": PASSWORD})[0], 404)
        self.assertEqual(request(
            port, "GET", "/api/v1/sync?cursor=0", pc_token)[1]["changes"][0]
            ["body"]["name"], "Bagimsiz mera")
        self._stop(process)

        # Explicit owner account is required; there is no default user.
        AccountStore(state / "owner_accounts.sqlite").bootstrap_owner(
            SITE, "owner", USER, PASSWORD)
        process2, port2 = self._start_server(
            state, "--enable-account-login")
        self.assertEqual(
            request(port2, "GET", "/api/v1/sync/identity", pc_token),
            (200, {"site": SITE, "installation": PC_ID}),
        )
        self.assertEqual(
            request(port2, "GET", "/api/v1/sync?cursor=0", pc_token)[0], 200)
        self.assertEqual(
            request(port2, "POST", "/api/v1/account/login", pc_token,
                    {"username": USER, "password": PASSWORD})[0], 401)
        self.assertEqual(
            request(port2, "GET", "/api/v1/sync/identity", None)[0], 401)
        self._stop(process2)


if __name__ == "__main__":
    unittest.main(verbosity=2)
