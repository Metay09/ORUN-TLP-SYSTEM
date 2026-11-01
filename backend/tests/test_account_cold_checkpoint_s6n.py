"""ORUN S6N cold-checkpoint tests, all file/state fixtures are synthetic.

Never runs systemctl or reads ORUN-SHARED-RUNTIME. The commit's real
maintenance flow is exercised with a mocked unit controller.
"""
from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import sqlite3
import subprocess
import sys
import unittest
from unittest import mock

from backend.tests.test_account_cold_cutover_preflight_s6m import (
    S6MReadOnlyPreflightTests,
)
from tools.ci.cold_checkpoint_shared_s6n import (
    ACKNOWLEDGEMENT, BACKEND, MIRROR, TIMER, OBSERVATIONS, OUTPUT,
    cold_checkpoint,
)
from tools.ci.shadow_account_sqlite_migration_s6j import (
    ShadowRefused, digest,
)
from tools.ci.stage_account_runtime_source_s6i import ROOT


class S6NColdCheckpointTests(unittest.TestCase):
    def setUp(self) -> None:
        fixture = S6MReadOnlyPreflightTests(
            methodName="test_pinned_old_units_and_active_observation_are_read_only")
        fixture.setUp()
        self.addCleanup(fixture.doCleanups)
        self.live = fixture.live
        self.candidate = fixture.candidate
        self.units = fixture.units
        self.fixture = fixture
        # S6M only checks private metadata; S6N must back up a real SQLite
        # mirror cache, so replace the dummy bytes in this synthetic fixture.
        cache = self.live / "state" / "pc" / "shared.sqlite"
        cache.unlink()
        with sqlite3.connect(cache) as db:
            db.execute("CREATE TABLE example (id INTEGER PRIMARY KEY, value TEXT)")
            db.execute("INSERT INTO example(value) VALUES ('test-only')")
        cache.chmod(0o600)
        self.states = {
            BACKEND: "ACTIVE", TIMER: "ACTIVE", MIRROR: "INACTIVE",
            OBSERVATIONS[0]: "ACTIVE", OBSERVATIONS[1]: "ACTIVE",
        }
        self.transitions = []

    def fake_ctl(self, action: str, unit: str):
        self.transitions.append((action, unit))
        self.assertIn(action, ("start", "stop"))
        self.assertIn(unit, (BACKEND, TIMER))
        self.states[unit] = "ACTIVE" if action == "start" else "INACTIVE"
        return subprocess.CompletedProcess([action, unit], 0, "", "")

    def invoke(self, *, execute=False, acknowledgement=None):
        with (mock.patch(
                  "tools.ci.cold_checkpoint_shared_s6n.inspect",
                  return_value=self.fixture.call()),
              mock.patch(
                  "tools.ci.cold_checkpoint_shared_s6n._systemctl",
                  side_effect=self.fake_ctl),
              mock.patch(
                  "tools.ci.cold_checkpoint_shared_s6n._state",
                  side_effect=lambda name: self.states[name])):
            return cold_checkpoint(
                self.live, self.candidate, self.units, root=ROOT,
                execute=execute, acknowledgement=acknowledgement)

    def test_default_mode_does_not_stop_any_service_or_create_files(self):
        self.assertIsNone(self.invoke())
        self.assertEqual(self.transitions, [])
        self.assertFalse((self.candidate / OUTPUT).exists())
        self.assertEqual(self.states[BACKEND], "ACTIVE")
        self.assertEqual(self.states[TIMER], "ACTIVE")

    def test_execute_without_exact_approval_does_not_stop_services(self):
        with self.assertRaisesRegex(
                ShadowRefused, "EXPLICIT_MAINTENANCE_APPROVAL_REQUIRED"):
            self.invoke(execute=True, acknowledgement="not-approved")
        self.assertEqual(self.transitions, [])
        self.assertFalse((self.candidate / OUTPUT).exists())

    def test_cold_checkpoint_contains_three_sqlite_and_token_then_resumes(self):
        secret = self.live / "state" / "secrets" / "pc-enrollment.token"
        original = secret.read_bytes()
        data = self.invoke(execute=True, acknowledgement=ACKNOWLEDGEMENT)
        self.assertEqual(data["state"], "SERVICE_PAUSED_BACKUP_NOT_ACTIVATED")
        self.assertEqual(data["account_login"], "NOT_ENABLED")
        self.assertEqual(self.transitions, [
            ("stop", TIMER), ("stop", BACKEND),
            ("start", BACKEND), ("start", TIMER),
        ])
        self.assertTrue(all(state == "ACTIVE" for name, state
                            in self.states.items() if name != MIRROR))
        bundle = self.candidate / OUTPUT / "bundle"
        self.assertEqual(
            (bundle / "state/secrets/pc-enrollment.token").read_bytes(),
            original,
        )
        self.assertEqual((bundle.parent.stat().st_mode & 0o777), 0o700)
        for name, value in data["files_sha256"].items():
            copied = bundle / name
            self.assertEqual(digest(copied), value)
            self.assertEqual(copied.stat().st_mode & 0o777, 0o600)
        for rel in data["sqlite_files"]:
            with sqlite3.connect(bundle / rel) as db:
                self.assertEqual(db.execute("PRAGMA quick_check").fetchone()[0],
                                 "ok")
        saved = json.loads(
            (bundle.parent / "CHECKPOINT_MANIFEST.json").read_text())
        self.assertEqual(saved, data)
        self.assertEqual(secret.read_bytes(), original)

    def test_failure_deletes_only_incomplete_snapshot_and_restarts_old_services(self):
        with mock.patch(
            "tools.ci.cold_checkpoint_shared_s6n._copy_snapshot",
            side_effect=ShadowRefused("SYNTHETIC_COPY_FAILURE"),
        ):
            with self.assertRaisesRegex(ShadowRefused, "SYNTHETIC_COPY_FAILURE"):
                self.invoke(execute=True, acknowledgement=ACKNOWLEDGEMENT)
        self.assertEqual(self.states[BACKEND], "ACTIVE")
        self.assertEqual(self.states[TIMER], "ACTIVE")
        self.assertFalse((self.candidate / OUTPUT).exists())
        self.assertEqual(self.transitions, [
            ("stop", TIMER), ("stop", BACKEND),
            ("start", BACKEND), ("start", TIMER),
        ])

    def test_restore_service_failure_marks_critical_and_preserves_backup(self):
        def fail_start(action: str, unit: str):
            if action == "start" and unit == BACKEND:
                return subprocess.CompletedProcess([action, unit], 1, "", "")
            return self.fake_ctl(action, unit)

        with mock.patch(
            "tools.ci.cold_checkpoint_shared_s6n.inspect",
            return_value=self.fixture.call(),
        ), mock.patch(
            "tools.ci.cold_checkpoint_shared_s6n._systemctl",
            side_effect=fail_start,
        ), mock.patch(
            "tools.ci.cold_checkpoint_shared_s6n._state",
            side_effect=lambda name: self.states[name],
        ):
            with self.assertRaisesRegex(
                    ShadowRefused, "CRITICAL_OLD_SERVICE_RESTART_REVIEW_REQUIRED"):
                cold_checkpoint(
                    self.live, self.candidate, self.units,
                    root=ROOT, execute=True, acknowledgement=ACKNOWLEDGEMENT)
        self.assertTrue(
            (self.candidate / OUTPUT / "CHECKPOINT_MANIFEST.json").is_file())
        self.assertEqual(self.states[BACKEND], "INACTIVE")

    def test_direct_python_import_does_not_depend_on_cwd(self):
        script = ROOT / "tools" / "ci" / "cold_checkpoint_shared_s6n.py"
        result = subprocess.run(
            [sys.executable, "-I", "-B", "-c",
             "import runpy; runpy.run_path(" + repr(str(script)) +
             ", run_name='s6n_import_only')"],
            cwd=self.live.parent, capture_output=True, text=True,
            timeout=10, check=False,
        )
        self.assertEqual(result.returncode, 0, result.stderr[:300])


if __name__ == "__main__":
    unittest.main(verbosity=2)
