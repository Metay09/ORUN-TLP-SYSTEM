"""ORUN S6M preflight tests using synthetic S4E/S6J snapshots.

No systemd command or live ORUN file is accessed in these tests.
"""
from __future__ import annotations

import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

import backend.tests.test_account_legacy_rollback_s6k as fixtures
from tools.ci.preflight_account_cold_cutover_s6m import (
    BACKEND, MIRROR, TIMER, OBSERVATIONS, inspect,
)
from tools.ci.shadow_account_sqlite_migration_s6j import ShadowRefused
from tools.ci.stage_account_runtime_source_s6i import ROOT


class S6MReadOnlyPreflightTests(unittest.TestCase):
    def setUp(self):
        fixture = fixtures.S6KLegacyRollbackReadTests(
            methodName="test_pinned_s4e_reads_migrated_shadow_and_preserves_all_sources")
        fixture.setUp()
        self.addCleanup(fixture.doCleanups)
        self.live = fixture.live
        self.candidate = fixture.candidate
        self.units = self.live.parent / "units"
        self.units.mkdir(mode=0o700)
        state = self.live / "state"
        for name in ("pc", "secrets"):
            directory = state / name
            directory.mkdir(mode=0o700)
            directory.chmod(0o700)
        for file in (
            state / "pc" / "shared.sqlite",
            state / "secrets" / "pc-enrollment.token",
        ):
            file.write_bytes(b"synthetic fixture not a credential")
            file.chmod(0o600)

        backend = self.live / "runtime" / "tools" / "orun_shared_sync_loopback.py"
        mirror = self.live / "runtime" / "tools" / "orun_shared_pc_mirror.py"
        self.texts = {
            BACKEND: (
                "[Service]\nExecStart=/usr/bin/python3 " + str(backend) +
                " --state-dir " + str(state / "backend") +
                " --bind 127.0.0.1 --port 18781 --reuse-state\n"),
            MIRROR: (
                "[Service]\nExecStart=/usr/bin/python3 " + str(mirror) +
                " --db " + str(state / "pc" / "shared.sqlite") +
                " --site orun-main --installation pc-main "
                "--url http://127.0.0.1:18781 --token-file " +
                str(state / "secrets" / "pc-enrollment.token") +
                " --once\n"),
            TIMER: (
                "[Timer]\nOnUnitInactiveSec=180s\n"
                "Unit=orun-shared-pc-mirror.service\n"),
        }
        for name, body in self.texts.items():
            p = self.units / name
            p.write_text(body)
            p.chmod(0o600)
        self.states = {
            "orun-backend.service": "active",
            "orun-pc-edge.service": "active",
            BACKEND: "active",
            MIRROR: "inactive",
            TIMER: "active",
        }
        self.calls = []

    def fake_systemctl(self, *args):
        self.calls.append(args)
        if args[:3] == ("show", "--property=FragmentPath", "--value"):
            return subprocess.CompletedProcess(
                args, 0, str(self.units / args[3]) + "\n", "")
        if args[0] == "is-active":
            state = self.states[args[1]]
            return subprocess.CompletedProcess(
                args, 0 if state == "active" else 3,
                state + "\n", "")
        raise AssertionError("read-only systemctl actions only")

    def call(self):
        with mock.patch(
            "tools.ci.preflight_account_cold_cutover_s6m._systemctl",
            side_effect=self.fake_systemctl,
        ):
            return inspect(self.live, self.candidate, self.units, root=ROOT)

    def test_pinned_old_units_and_active_observation_are_read_only(self):
        output = self.call()
        self.assertEqual(output["S6M_PINNED_UNITS_AND_SHADOW"], "VERIFIED")
        self.assertEqual(output["COLD_CHECKPOINT_READY"],
                         "NO_NOT_QUIESCED")
        self.assertEqual(output["LIVE_SERVICE_CHANGE"], "NOT_ATTEMPTED")
        self.assertFalse(any(args[0] not in ("show", "is-active")
                             for args in self.calls))

    def test_mirror_running_requires_wait_not_claim_quiescence(self):
        self.states[MIRROR] = "active"
        with self.assertRaisesRegex(ShadowRefused, "MIRROR_ONESHOT_BUSY"):
            self.call()

    def test_unexpected_installed_backend_or_loaded_unit_rejected(self):
        p = self.units / BACKEND
        original = p.read_text()
        p.write_text(original + "# --enable-account-login\n")
        # A comment alone cannot activate account login, but the original
        # installed executable must still be verified without trusting it.
        self.assertEqual(self.call()["PRODUCTION_APPROVAL"], "NO")
        p.write_text(original.replace(" --port 18781 ", " --port 18782 "))
        with self.assertRaisesRegex(ShadowRefused, "BACKEND_UNIT_MISMATCH"):
            self.call()
        p.write_text(original.replace(
            " --reuse-state", " --reuse-state --enable-account-login"))
        with self.assertRaisesRegex(ShadowRefused, "BACKEND_UNIT_MISMATCH"):
            self.call()
        p.write_text(original)
        with mock.patch(
            "tools.ci.preflight_account_cold_cutover_s6m._systemctl",
            side_effect=lambda *args: (
                subprocess.CompletedProcess(args, 0, "/tmp/untrusted.service\n", "")
                if args[0] == "show" else self.fake_systemctl(*args)),
        ):
            with self.assertRaisesRegex(ShadowRefused, "LOADED_UNIT_PATH_MISMATCH"):
                inspect(self.live, self.candidate, self.units, root=ROOT)

    def test_observation_service_not_active_blocks_gate(self):
        self.states[OBSERVATIONS[1]] = "inactive"
        with self.assertRaisesRegex(ShadowRefused, "OBSERVATION_SERVICE"):
            self.call()

    def test_direct_entrypoint_imports_without_project_pythonpath(self):
        script = (ROOT / "tools" / "ci" /
                  "preflight_account_cold_cutover_s6m.py")
        result = subprocess.run(
            [sys.executable, "-I", "-B", "-c",
             "import runpy; runpy.run_path(" + repr(str(script)) +
             ", run_name='s6m_import_only')"],
            cwd=self.live.parent, capture_output=True, text=True,
            timeout=10, check=False,
        )
        self.assertEqual(result.returncode, 0, result.stderr[:400])
        self.assertNotIn("ModuleNotFoundError", result.stderr)

    def test_missing_or_world_readable_credential_metadata_rejected(self):
        secret = self.live / "state" / "secrets" / "pc-enrollment.token"
        secret.chmod(0o644)
        with self.assertRaisesRegex(ShadowRefused, "PRIVATE_FILE_UNSAFE"):
            self.call()
        secret.chmod(0o600)
        secret.unlink()
        secret.symlink_to(self.live / "STAGING_MANIFEST.json")
        # Symlinks and group/world-readable secrets now share the same
        # metadata-only refusal path; never open or follow the token file.
        with self.assertRaisesRegex(ShadowRefused, "PRIVATE_FILE_UNSAFE"):
            self.call()


if __name__ == "__main__":
    unittest.main(verbosity=2)
