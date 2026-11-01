#!/usr/bin/env python3
"""ORUN S6A owner-account bootstrap: trusted LOCAL operator only.

No default password. Never accept password through argv, environment, URL,
JSON file, terminal echo, or support-chat messages. New accounts cannot be
created by the untrusted HTTP API. No existing account is overwritten.
"""
from __future__ import annotations

import argparse
import getpass
import os
from pathlib import Path
import stat
import sys

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from backend.account_login import AccountError, AccountStore


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Explicit private ORUN owner account bootstrap")
    parser.add_argument("--state-dir", required=True,
                        help="Dedicated, existing user-owned 0700 state directory")
    parser.add_argument("--site", required=True)
    parser.add_argument("--actor", required=True,
                        help="must match the owner actor used in phone approval")
    parser.add_argument("--username", required=True)
    parser.add_argument("--confirm-new-owner", action="store_true")
    args = parser.parse_args()
    if not args.confirm_new_owner:
        print("ORUN_ACCOUNT_REFUSED=OWNER_CONFIRMATION_REQUIRED", file=sys.stderr)
        return 2
    state = Path(args.state_dir)
    try:
        info = state.lstat()
        if (not stat.S_ISDIR(info.st_mode) or
                info.st_uid != os.geteuid() or
                stat.S_IMODE(info.st_mode) & 0o077):
            raise AccountError("private directory required")
        destination = state / "owner_accounts.sqlite"
        if destination.is_symlink():
            raise AccountError("unsafe account database symlink")
        if destination.exists():
            raise AccountError("existing account store requires separate recovery flow")
        password = getpass.getpass("Yeni ORUN hesabı parolası (gizli): ")
        verify = getpass.getpass("Parolayı tekrar gir (gizli): ")
        if password != verify:
            print("ORUN_ACCOUNT_REFUSED=PASSWORD_MISMATCH", file=sys.stderr)
            return 2
        os.umask(0o077)
        store = AccountStore(destination)
        store.bootstrap_owner(
            args.site, args.actor, args.username, password)
        print("ORUN_OWNER_ACCOUNT_BOOTSTRAP=PASS")
        print("Parola kaydedilmedi; yalnız salt+hash saklandı.")
        return 0
    except (AccountError, OSError, EOFError, KeyboardInterrupt):
        print("ORUN_ACCOUNT_REFUSED=PRIVATE_BOOTSTRAP_FAILED", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
