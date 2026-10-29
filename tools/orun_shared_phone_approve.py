#!/usr/bin/env python3
"""Trusted OWNER shell approval of a pending ORUN phone's exact key fingerprint.

This is *not* an HTTP handler and it never touches Gateway/Tracker authority,
observation uploads, PC Edge or existing user accounts.
The one-time secret must be entered directly into the correct phone; NEVER
paste it into a support chat or share screenshots of the terminal.
"""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from backend.edge_enrollment import EnrollmentError
from tools.orun_shared_sync_loopback import StartupRefused, prepare

VIEW = frozenset({"zone:read", "group:read", "zone_target:read"})
EDIT = frozenset({"zone:read", "zone:write", "group:read", "group:write",
                  "zone_target:read", "zone_target:write"})


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Approve a verified phone key via trusted local operator only")
    parser.add_argument("--state-dir", required=True)
    parser.add_argument("--request-id", required=True)
    parser.add_argument("--fingerprint", required=True,
                        help="must match fingerprint visible independently on phone")
    parser.add_argument("--site", required=True)
    parser.add_argument("--actor", required=True)
    parser.add_argument("--access", choices=("read-only", "editor"), required=True)
    parser.add_argument("--approve-verified-phone", action="store_true",
                        help="explicit confirmation that request ID and key fingerprint were matched")
    args = parser.parse_args()
    if not args.approve_verified_phone:
        print("PHONE_APPROVAL_REFUSED=EXPLICIT_OWNER_CONFIRMATION_REQUIRED",
              file=sys.stderr)
        return 2
    os.umask(0o077)
    try:
        _, registry = prepare(Path(args.state_dir), reuse_state=True)
        code = registry.approve(
            args.request_id, args.fingerprint, args.site, args.actor,
            EDIT if args.access == "editor" else VIEW)
    except (EnrollmentError, StartupRefused, OSError) as exc:
        print("PHONE_APPROVAL_REFUSED=" + type(exc).__name__, file=sys.stderr)
        return 2
    # Intentional, direct trusted-operator display: an approval code is
    # needed on the phone. Never send this output to ChatGPT/GitHub/logs.
    print("PHONE_OWNER_APPROVED=YES")
    print("PHONE_ACCESS=" + args.access)
    print("PHONE_ONE_TIME_APPROVAL_CODE=" + code)
    print("SECRET: ENTER ONLY ON THE PHONE. DO NOT SHARE THIS CODE.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
