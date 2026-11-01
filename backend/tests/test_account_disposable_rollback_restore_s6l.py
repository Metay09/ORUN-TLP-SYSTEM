"""S6L disposable rollback acceptance. All restored copies are synthetic."""
from __future__ import annotations

import contextlib
import io
from pathlib import Path
import subprocess
import sys
import unittest

from backend.tests.test_account_legacy_rollback_s6k import (
    S6KLegacyRollbackReadTests,
)
from tools.ci.rehearse_account_rollback_restore_s6l import rehearse
from tools.ci.shadow_account_sqlite_migration_s6j import (
    ShadowRefused, digest,
)
from tools.ci.stage_account_runtime_source_s6i import ROOT


class S6LDisposableRollbackRestoreTests(unittest.TestCase):
    def setUp(self):
        fixture = S6KLegacyRollbackReadTests(
            methodName="test_pinned_s4e_reads_migrated_shadow_and_preserves_all_sources")
        fixture.setUp()
        self.addCleanup(fixture.doCleanups)
        self.live = fixture.live
        self.candidate = fixture.candidate

    def _hashes(self):
        paths = list((self.live / "state" / "backend").glob("*.sqlite"))
        paths.extend((self.candidate / "SHADOW_S6J").rglob("*.sqlite"))
        return {str(p): digest(p) for p in paths}

    def test_restores_original_schema_and_ledger_after_synthetic_writes(self):
        before = self._hashes()
        with contextlib.redirect_stdout(io.StringIO()) as output:
            rehearse(self.live, self.candidate, root=ROOT)
        self.assertIn(
            "ORUN_S6L_DISPOSABLE_ROLLBACK_RESTORE_PASS",
            output.getvalue())
        self.assertEqual(self._hashes(), before)
        self.assertEqual(list(self.candidate.glob("S6L_COLD_RESTORE_TEST_*")),
                         [])

    def test_tampered_checkpoint_input_is_rejected_without_new_copy(self):
        path = (self.candidate / "SHADOW_S6J" / "baseline" /
                "enrolled_edges.sqlite")
        with path.open("ab") as f:
            f.write(b"tamper")
        with self.assertRaisesRegex(ShadowRefused, "SHADOW_DATABASE_HASH"):
            rehearse(self.live, self.candidate, root=ROOT)
        self.assertEqual(list(self.candidate.glob("S6L_COLD_RESTORE_TEST_*")),
                         [])

    def test_direct_cli_outside_repo_works_with_isolated_python(self):
        before = self._hashes()
        script = ROOT / "tools" / "ci" / "rehearse_account_rollback_restore_s6l.py"
        result = subprocess.run(
            [sys.executable, "-I", "-B", str(script),
             "--live-root", str(self.live),
             "--candidate", str(self.candidate)],
            cwd=self.live.parent,
            capture_output=True, text=True,
            timeout=30, check=False,
        )
        self.assertEqual(result.returncode, 0, result.stderr[:1000])
        self.assertIn("ORUN_S6L_DISPOSABLE_ROLLBACK_RESTORE_PASS",
                      result.stdout)
        self.assertNotIn("ModuleNotFoundError", result.stderr)
        self.assertEqual(before, self._hashes())
        self.assertEqual(list(self.candidate.glob("S6L_COLD_RESTORE_TEST_*")),
                         [])


if __name__ == "__main__":
    unittest.main(verbosity=2)
