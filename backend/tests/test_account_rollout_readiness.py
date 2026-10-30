"""Isolated S6G tests. No existing ORUN files or systemd mutations."""
from __future__ import annotations

import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest import mock

from tools.ci.audit_account_rollout_readiness import (
    active_service, inspect, source_status, state_status, unit_status,
)

class AccountRolloutInventoryTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="orun-s6g-audit-")
        self.addCleanup(self.temp.cleanup)
        base = Path(self.temp.name)
        self.root = base / "ORUN-SHARED-RUNTIME"
        self.units = base / "units"
        for p in (
            self.root,
            self.root / "runtime",
            self.root / "runtime" / "tools",
            self.root / "state",
            self.root / "state" / "backend",
            self.units,
        ):
            p.mkdir(mode=0o700, parents=True, exist_ok=True)
            p.chmod(0o700)
        self.uid = os.geteuid()
        self.source = (self.root / "runtime" / "tools" /
                       "orun_shared_sync_loopback.py")
        self.accounts = (self.root / "state" / "backend" /
                         "owner_accounts.sqlite")
        self.unit = self.units / "orun-shared-sync.service"
        self.source.write_text("# old pinned\n", encoding="utf-8")
        self.source.chmod(0o600)
        self.unit.write_text(
            "[Service]\nExecStart=/usr/bin/python3 tools/orun_shared_sync_loopback.py\n",
            encoding="utf-8",
        )
        self.accounts.write_bytes(b"synthetic fixture, never a real SQLite")
        self.accounts.chmod(0o600)

    def test_old_live_runtime_is_not_reported_as_account_ready(self):
        result = inspect(self.root, self.units, self.uid)
        self.assertEqual(result, {
            "PINNED_BACKEND_SOURCE": "OLD_OR_DIFFERENT_PINNED_SOURCE",
            "INSTALLED_ACCOUNT_FLAG": "ACCOUNT_FLAG_ABSENT",
            "OWNER_ACCOUNT_STORE": "PRESENT",
        })
        self.assertEqual(self.accounts.read_bytes(),
                         b"synthetic fixture, never a real SQLite")

    def test_new_source_does_not_imply_existing_service_was_switched(self):
        self.source.write_text(
            'parser.add_argument("--enable-account-login", action="store_true")\n'
            "account_handler_factory\n",
            encoding="utf-8",
        )
        self.assertEqual(source_status(self.root, self.uid),
                         "STATIC_ACCOUNT_FLAG_FOUND")
        self.assertEqual(unit_status(self.units), "ACCOUNT_FLAG_ABSENT")
        self.unit.write_text(
            "[Service]\n# ExecStart=/bin/old --enable-account-login\n"
            "ExecStart=/usr/bin/python3 example.py --enable-account-login\n",
            encoding="utf-8",
        )
        self.assertEqual(unit_status(self.units), "ACCOUNT_FLAG_PRESENT")

    def test_private_owner_account_file_is_required_and_never_opened(self):
        self.accounts.unlink()
        self.assertEqual(state_status(self.root, self.uid), "MISSING")
        self.accounts.write_text("do not parse", encoding="utf-8")
        self.accounts.chmod(0o644)
        self.assertEqual(state_status(self.root, self.uid), "UNSAFE")
        self.accounts.unlink()
        self.accounts.symlink_to(self.source)
        self.assertEqual(state_status(self.root, self.uid), "UNSAFE")

    def test_symlinked_pinned_runtime_is_not_followed(self):
        self.source.unlink()
        self.source.symlink_to(self.accounts)
        self.assertEqual(source_status(self.root, self.uid), "NOT_VERIFIED")
        self.source.unlink()
        self.source.write_text("# old", encoding="utf-8")
        self.source.chmod(0o600)
        (self.root / "runtime" / "tools").chmod(0o755)
        self.assertEqual(source_status(self.root, self.uid), "NOT_VERIFIED")

    def test_systemctl_read_only_status_never_prints_service_details(self):
        ok = subprocess.CompletedProcess(args=[], returncode=0,
                                         stdout="active\n", stderr="private")
        with mock.patch(
            "tools.ci.audit_account_rollout_readiness.subprocess.run",
            return_value=ok,
        ) as run:
            self.assertEqual(active_service("orun-shared-sync.service"), "ACTIVE")
            called = run.call_args.args[0]
            self.assertEqual(called, [
                "systemctl", "--user", "is-active",
                "orun-shared-sync.service",
            ])
        with mock.patch(
            "tools.ci.audit_account_rollout_readiness.subprocess.run",
            return_value=subprocess.CompletedProcess(
                args=[], returncode=3, stdout="inactive", stderr="secret"
            ),
        ):
            self.assertEqual(active_service("orun-shared-sync.service"),
                             "NOT_ACTIVE")

if __name__ == "__main__":
    unittest.main()
