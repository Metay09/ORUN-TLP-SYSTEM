"""S4B: no service installation. Validate credential-file and timer staging."""
from __future__ import annotations

import os
from pathlib import Path
import stat
import tempfile
import unittest

from tools.orun_shared_pc_mirror import _load_private_token, MirrorError
from tools.ci.render_shared_pc_timer import render, UNIT, TIMER


class SharedPcTimerTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="orun-pc-timer-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.repo = Path(__file__).resolve().parents[2]
        self.staging = self.root / "staging"
        self.staging.mkdir(mode=0o700)
        self.db = self.root / "separate-pc-shared-mirror.sqlite"
        self.secret = self.root / "per-installation-token"
        self.token = "A" * 64
        self.secret.write_text(self.token + "\n", encoding="ascii")
        os.chmod(self.secret, 0o600)

    def test_token_file_private_and_no_implicit_bench_fallback(self):
        self.assertEqual(_load_private_token(str(self.secret)), self.token)
        os.chmod(self.secret, 0o644)
        with self.assertRaisesRegex(MirrorError, "SYNC_TOKEN_FILE_PERMISSIONS"):
            _load_private_token(str(self.secret))
        os.chmod(self.secret, 0o600)
        symlink = self.root / "token-symlink"
        symlink.symlink_to(self.secret)
        with self.assertRaisesRegex(MirrorError, "SYNC_TOKEN_FILE_INVALID"):
            _load_private_token(str(symlink))
        self.secret.write_text("shared-bench-token", encoding="ascii")
        with self.assertRaisesRegex(MirrorError, "SYNC_TOKEN_FILE_INVALID"):
            _load_private_token(str(self.secret))

    def test_renderer_stages_disabled_unit_without_secrets(self):
        service, timer = render(
            self.repo, self.staging, self.db, "site-A", "pc-edge",
            "https://orun.example", self.secret)
        self.assertEqual(service, self.staging / UNIT)
        self.assertEqual(timer, self.staging / TIMER)
        self.assertEqual(stat.S_IMODE(service.stat().st_mode), 0o600)
        self.assertEqual(stat.S_IMODE(timer.stat().st_mode), 0o600)
        unit = service.read_text(encoding="utf-8")
        schedule = timer.read_text(encoding="utf-8")
        self.assertIn("ExecStart=", unit)
        self.assertIn("--token-file", unit)
        self.assertIn("--once", unit)
        self.assertIn("OnUnitInactiveSec=180s", schedule)
        self.assertIn("Persistent=false", schedule)
        self.assertNotIn(self.token, unit + schedule)
        self.assertNotIn("systemctl", unit + schedule)
        self.assertNotIn("Restart=always", unit + schedule)
        self.assertFalse(self.db.exists(), "render cannot initialize the local database")
        self.assertEqual(_load_private_token(str(self.secret)), self.token)
        with self.assertRaises(FileExistsError):
            render(self.repo, self.staging, self.db, "site-A", "pc-edge",
                   "https://orun.example", self.secret)
        self.assertEqual(service.read_text(encoding="utf-8"), unit,
                         "render refuses overwrites and leaves original intact")

    def test_renderer_fails_closed_for_insecure_input(self):
        for bad in ("http://100.114.36.9:8090", "http://localhost:9000",
                    "https://example.com/?token=abc"):
            with self.subTest(bad=bad), self.assertRaises(MirrorError):
                render(self.repo, self.staging, self.db, "site-A", "pc-edge", bad,
                       self.secret)
        for site in ("not a site", "evil;rm", "a$USER", "bad%p"):
            with self.subTest(site=site), self.assertRaises(ValueError):
                render(self.repo, self.staging, self.db, site, "pc-edge",
                       "https://orun.example", self.secret)
        self.assertFalse((self.staging / UNIT).exists())
        self.assertFalse((self.staging / TIMER).exists())


if __name__ == "__main__":
    unittest.main(verbosity=2)
