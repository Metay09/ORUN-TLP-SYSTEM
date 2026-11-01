#!/usr/bin/env python3
"""ORUN S6G: strictly read-only local account rollout inventory.

No state SQLite open, credential/token read, HTTP request, systemd mutation,
APK build, Tailscale change, or filesystem write. This is NOT deployment
approval. No readiness report may be treated as authorization to switch
live services.
"""
from __future__ import annotations

import os
from pathlib import Path
import re
import stat
import subprocess

ROOT_ENV = "ORUN_SHARED_RUNTIME_ROOT"
UNITS = (
    "orun-backend.service",
    "orun-pc-edge.service",
    "orun-shared-sync.service",
    "orun-shared-pc-mirror.timer",
)

def safe_file(path: Path, uid: int, *, private: bool) -> str:
    try:
        meta = path.lstat()
    except FileNotFoundError:
        return "MISSING"
    except OSError:
        return "INACCESSIBLE"
    if not stat.S_ISREG(meta.st_mode) or meta.st_uid != uid:
        return "UNSAFE"
    if private and stat.S_IMODE(meta.st_mode) & 0o077:
        return "UNSAFE"
    return "PRESENT"

def safe_directory(path: Path, uid: int) -> str:
    try:
        meta = path.lstat()
    except FileNotFoundError:
        return "MISSING"
    except OSError:
        return "INACCESSIBLE"
    if not stat.S_ISDIR(meta.st_mode) or meta.st_uid != uid or (
        stat.S_IMODE(meta.st_mode) & 0o077
    ):
        return "UNSAFE"
    return "PRESENT"

def source_status(runtime: Path, uid: int) -> str:
    parent = runtime / "runtime" / "tools"
    if safe_directory(runtime, uid) != "PRESENT" or (
        safe_directory(runtime / "runtime", uid) != "PRESENT"
    ) or safe_directory(parent, uid) != "PRESENT":
        return "NOT_VERIFIED"
    script = parent / "orun_shared_sync_loopback.py"
    if safe_file(script, uid, private=True) != "PRESENT":
        return "NOT_VERIFIED"
    try:
        data = script.read_text(encoding="utf-8")
    except (OSError, UnicodeError):
        return "NOT_VERIFIED"
    # Static feature presence ONLY, not a runtime handshake or rollout approval.
    return "STATIC_ACCOUNT_FLAG_FOUND" if (
        'parser.add_argument("--enable-account-login"' in data and
        'account_handler_factory' in data
    ) else "OLD_OR_DIFFERENT_PINNED_SOURCE"

def unit_status(units: Path) -> str:
    unit = units / "orun-shared-sync.service"
    try:
        if not stat.S_ISREG(unit.lstat().st_mode):
            return "NOT_VERIFIED"
        text = unit.read_text(encoding="utf-8")
    except (FileNotFoundError, OSError, UnicodeError):
        return "NOT_VERIFIED"
    # Only uncommented ExecStart lines count. Never print process arguments.
    starts = [line.strip() for line in text.splitlines()
              if re.match(r"^\s*ExecStart\s*=", line)]
    if len(starts) != 1:
        return "NOT_VERIFIED"
    return "ACCOUNT_FLAG_PRESENT" if (
        "--enable-account-login" in starts[0]
    ) else "ACCOUNT_FLAG_ABSENT"

def state_status(runtime: Path, uid: int) -> str:
    for dirname in (runtime, runtime / "state", runtime / "state" / "backend"):
        state = safe_directory(dirname, uid)
        if state != "PRESENT":
            return state
    return safe_file(runtime / "state" / "backend" / "owner_accounts.sqlite",
                     uid, private=True)

def active_service(name: str) -> str:
    try:
        result = subprocess.run(
            ["systemctl", "--user", "is-active", name], capture_output=True,
            text=True, timeout=5, check=False,
        )
    except (OSError, subprocess.TimeoutExpired):
        return "UNKNOWN"
    # Do not surface stderr/stdout: service errors can contain sensitive data.
    return "ACTIVE" if result.returncode == 0 and (
        result.stdout.strip() == "active"
    ) else "NOT_ACTIVE"

def inspect(runtime: Path, units: Path, uid: int) -> dict[str, str]:
    return {
        "PINNED_BACKEND_SOURCE": source_status(runtime, uid),
        "INSTALLED_ACCOUNT_FLAG": unit_status(units),
        "OWNER_ACCOUNT_STORE": state_status(runtime, uid),
    }

def main() -> int:
    home = Path.home()
    # Fixed location of the S4E installed snapshot, never the development tree.
    runtime = home / "ORUN-SHARED-RUNTIME"
    # Do not accept attacker-provided override pointing at sensitive files.
    units = home / ".config" / "systemd" / "user"
    print("=== ORUN S6G LIVE ACCOUNT READ-ONLY PREFLIGHT ===")
    for service in UNITS:
        print("SERVICE_" + service.replace(".", "_").replace("-", "_").upper()
              + "=" + active_service(service))
    for key, value in inspect(runtime, units, os.geteuid()).items():
        print(key + "=" + value)
    print("ACCOUNT_ENDPOINT_ACTIVATION=NOT_ATTEMPTED")
    print("SERVICE_RESTART=NOT_ATTEMPTED")
    print("PRODUCTION_APPROVAL=NO")
    print("NOTE=Static checks only; physical Android and rollback gates pending")
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
