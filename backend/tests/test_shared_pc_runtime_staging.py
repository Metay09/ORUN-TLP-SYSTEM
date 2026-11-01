"""S4E stable runtime staging gate: no systemd changes, no live data.

Tests run only in TemporaryDirectory; real isolated S4C subprocess and PC
read-only HTTP mirror use private 0600 token created by local owner consent.
"""
from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import select
import socket
import stat
import subprocess
import sys
import tempfile
import unittest

from tools.ci.stage_shared_pc_runtime import stage, StageRefused, SOURCES
from tools.ci.render_shared_pc_timer import UNIT as MIRROR_SERVICE, TIMER
from tools.ci.stage_shared_pc_runtime import SERVICE as BACKEND_SERVICE


def free_port() -> int:
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


class SidecarStagingTests(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory(prefix="orun-s4e-")
        self.addCleanup(temp.cleanup)
        self.tmp = Path(temp.name)
        self.staged = self.tmp / "shared-private"
        self.port = free_port()

    def test_refuses_unapproved_or_existing_destination_without_touching_files(self):
        with self.assertRaisesRegex(StageRefused, "EXPLICIT_LOCAL_PC_APPROVAL_REQUIRED"):
            stage(self.staged, "site-a", "pc-main", approved=False, port=self.port)
        self.assertFalse(self.staged.exists())
        self.staged.mkdir(mode=0o700)
        marker = self.staged / "KEEP_THIS"
        marker.write_text("KEEP_THIS")
        with self.assertRaisesRegex(StageRefused, "STAGING_NEW_ABSOLUTE_DIRECTORY_REQUIRED"):
            stage(self.staged, "site-a", "pc-main", approved=True, port=self.port)
        self.assertEqual(marker.read_text(), "KEEP_THIS")
        self.assertEqual(list(self.staged.iterdir()), [marker])

    def test_real_pinned_runtime_backend_and_pc_http_without_unit_activation(self):
        details = stage(self.staged, "pilot-a", "pc-main",
                        approved=True, port=self.port)
        self.assertEqual(details["runtime_state"], "STAGED_NOT_INSTALLED")
        self.assertEqual(set(details["source_sha256"]), set(SOURCES))
        for rel in SOURCES:
            copied = self.staged / "runtime" / rel
            self.assertEqual(hashlib.sha256(copied.read_bytes()).hexdigest(),
                             details["source_sha256"][rel])
            self.assertEqual(stat.S_IMODE(copied.stat().st_mode), 0o600)
        for path in [self.staged, self.staged / "state",
                     self.staged / "state" / "backend",
                     self.staged / "state" / "pc",
                     self.staged / "state" / "secrets",
                     self.staged / "units"]:
            self.assertEqual(stat.S_IMODE(path.stat().st_mode), 0o700)

        token_file = self.staged / "state/secrets/pc-enrollment.token"
        secret = token_file.read_text().strip()
        self.assertGreaterEqual(len(secret), 48)
        self.assertEqual(stat.S_IMODE(token_file.stat().st_mode), 0o600)
        unit_data = [(self.staged / "units" / x).read_text() for x in
                     [BACKEND_SERVICE, MIRROR_SERVICE, TIMER]]
        all_units = "\n".join(unit_data)
        self.assertNotIn(secret, all_units)
        self.assertNotIn("8090", all_units)
        self.assertNotIn("systemctl ", all_units)
        self.assertIn("Requires=orun-shared-sync.service", unit_data[1])
        self.assertIn(f"--port {self.port} --reuse-state", unit_data[0])
        self.assertIn("OnUnitInactiveSec=180s", unit_data[2])
        self.assertIn("STAGED_NOT_INSTALLED",
                      (self.staged / "STAGING_MANIFEST.json").read_text())

        runtime = self.staged / "runtime"
        cmd = [sys.executable, "-u",
               str(runtime / "tools" / "orun_shared_sync_loopback.py"),
               "--state-dir", str(self.staged / "state/backend"),
               "--port", str(self.port), "--reuse-state"]
        server = subprocess.Popen(cmd, stdout=subprocess.PIPE,
                                  stderr=subprocess.PIPE, text=True,
                                  cwd=self.tmp, env={**os.environ,
                                  "PYTHONDONTWRITEBYTECODE": "1"})
        try:
            assert server.stdout is not None
            ready, _, _ = select.select([server.stdout], [], [], 10)
            self.assertTrue(ready, "staged loopback Backend failed to start")
            line = server.stdout.readline().strip()
            self.assertEqual(line, f"ORUN_SHARED_BIND=127.0.0.1:{self.port}")

            pull = subprocess.run([
                sys.executable, str(runtime / "tools" / "orun_shared_pc_mirror.py"),
                "--db", str(self.staged / "state/pc/shared.sqlite"),
                "--site", "pilot-a", "--installation", "pc-main",
                "--url", f"http://127.0.0.1:{self.port}",
                "--token-file", str(token_file), "--once",
            ], capture_output=True, text=True, timeout=15, cwd=self.tmp)
            self.assertEqual(pull.returncode, 0, pull.stderr)
            self.assertEqual(json.loads(pull.stdout)["received"], 0)
            self.assertNotIn(secret, pull.stdout + pull.stderr)
        finally:
            server.terminate()
            try:
                server.communicate(timeout=5)
            except subprocess.TimeoutExpired:
                server.kill()
                server.communicate(timeout=5)


if __name__ == "__main__":
    unittest.main(verbosity=2)
