#!/usr/bin/env python3
"""ORUN S6M: read-only operator preflight for a future cold cutover.

No systemd stop/start/daemon-reload, no HTTP, no SQLite open/write/backup,
no token reads, no new account creation and no install. Verify installed
source/units and identify precise quiesce requirements only.

This is NOT an activation procedure or permission to stop running services.
"""
from __future__ import annotations

import os
from pathlib import Path
import shlex
import stat
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from tools.ci.probe_s4e_shadow_rollback_s6k import validate
from tools.ci.shadow_account_sqlite_migration_s6j import (
    INSTALLATION, PORT, SITE, ShadowRefused,
)

BACKEND = "orun-shared-sync.service"
MIRROR = "orun-shared-pc-mirror.service"
TIMER = "orun-shared-pc-mirror.timer"
OBSERVATIONS = ("orun-backend.service", "orun-pc-edge.service")
UNITS = (BACKEND, MIRROR, TIMER)


def _systemctl(*args: str) -> subprocess.CompletedProcess[str]:
    try:
        return subprocess.run(
            ["systemctl", "--user", *args],
            capture_output=True, text=True, timeout=6, check=False,
        )
    except (OSError, subprocess.TimeoutExpired):
        raise ShadowRefused("SYSTEMD_READ_UNAVAILABLE") from None


def _state(unit: str) -> str:
    result = _systemctl("is-active", unit)
    state = result.stdout.strip()
    if state == "active" and result.returncode == 0:
        return "ACTIVE"
    if state == "inactive" and result.returncode in (0, 3):
        return "INACTIVE"
    raise ShadowRefused("UNEXPECTED_SYSTEMD_STATE_" +
                        unit.replace("-", "_").replace(".", "_"))


def _private_unit(path: Path) -> str:
    try:
        info = path.lstat()
        if (not stat.S_ISREG(info.st_mode) or
                info.st_uid != os.geteuid() or
                stat.S_IMODE(info.st_mode) & 0o077):
            raise ShadowRefused("INSTALLED_UNIT_UNSAFE")
        return path.read_text(encoding="utf-8")
    except (OSError, UnicodeError):
        raise ShadowRefused("INSTALLED_UNIT_MISSING") from None


def _loaded_identity(name: str, expected: Path) -> None:
    response = _systemctl("show", "--property=FragmentPath",
                          "--value", name)
    actual = response.stdout.strip()
    if (response.returncode or actual != str(expected) or
            not expected.is_absolute()):
        raise ShadowRefused("LOADED_UNIT_PATH_MISMATCH_" +
                            name.replace("-", "_").replace(".", "_"))


def _start_tokens(data: str) -> list[str]:
    lines = [line.split("=", 1)[1].strip() for line in data.splitlines()
             if line.strip().startswith("ExecStart=")]
    if len(lines) != 1 or not lines[0]:
        raise ShadowRefused("INSTALLED_EXECSTART_UNEXPECTED")
    try:
        tokens = shlex.split(lines[0])
    except ValueError:
        raise ShadowRefused("INSTALLED_EXECSTART_INVALID") from None
    if len(tokens) < 2:
        raise ShadowRefused("INSTALLED_EXECSTART_INVALID")
    return tokens


def _flag(tokens: list[str], name: str) -> str:
    if tokens.count(name) != 1:
        raise ShadowRefused("INSTALLED_UNIT_FLAG_MISSING")
    index = tokens.index(name)
    if index + 1 >= len(tokens):
        raise ShadowRefused("INSTALLED_UNIT_FLAG_MISSING")
    return tokens[index + 1]


def verify_installed_units(live: Path, units: Path) -> None:
    # Metadata-only check for the PC mirror and its secret holder. Never
    # inspect their bytes or print any token/path in output.
    for folder in (live / "state" / "pc", live / "state" / "secrets"):
        try:
            info = folder.lstat()
        except OSError:
            raise ShadowRefused("PRIVATE_MIRROR_STATE_MISSING") from None
        if (not stat.S_ISDIR(info.st_mode) or info.st_uid != os.geteuid() or
                stat.S_IMODE(info.st_mode) & 0o077):
            raise ShadowRefused("PRIVATE_MIRROR_STATE_UNSAFE")
    for item in (live / "state" / "pc" / "shared.sqlite",
                 live / "state" / "secrets" / "pc-enrollment.token"):
        _private_unit(item)
    server = _private_unit(units / BACKEND)
    mirror = _private_unit(units / MIRROR)
    timer = _private_unit(units / TIMER)
    for unit in UNITS:
        _loaded_identity(unit, units / unit)

    server_tokens = _start_tokens(server)
    mirror_tokens = _start_tokens(mirror)
    server_script = live / "runtime" / "tools" / "orun_shared_sync_loopback.py"
    mirror_script = live / "runtime" / "tools" / "orun_shared_pc_mirror.py"
    if (str(server_script) not in server_tokens or
            str(mirror_script) not in mirror_tokens or
            "--reuse-state" not in server_tokens or
            "--enable-account-login" in server_tokens or
            "--enable-phone-enrollment" in server_tokens or
            _flag(server_tokens, "--state-dir") != str(live / "state" / "backend") or
            _flag(server_tokens, "--bind") != "127.0.0.1" or
            _flag(server_tokens, "--port") != str(PORT)):
        raise ShadowRefused("INSTALLED_BACKEND_UNIT_MISMATCH")

    if (_flag(mirror_tokens, "--db") !=
            str(live / "state" / "pc" / "shared.sqlite") or
            _flag(mirror_tokens, "--site") != SITE or
            _flag(mirror_tokens, "--installation") != INSTALLATION or
            _flag(mirror_tokens, "--url") != f"http://127.0.0.1:{PORT}" or
            _flag(mirror_tokens, "--token-file") !=
            str(live / "state" / "secrets" / "pc-enrollment.token") or
            "--once" not in mirror_tokens):
        raise ShadowRefused("INSTALLED_MIRROR_UNIT_MISMATCH")

    meaningful = [line.strip() for line in timer.splitlines()
                  if line.strip() and not line.lstrip().startswith("#")]
    if ("Unit=" + MIRROR not in meaningful or
            "OnUnitInactiveSec=180s" not in meaningful):
        raise ShadowRefused("INSTALLED_TIMER_UNIT_MISMATCH")


def inspect(live: Path, candidate: Path, units: Path,
            *, root: Path = ROOT) -> dict[str, str]:
    # This validates old/new source SHA256 and S6J shadow integrity; it never
    # opens live SQLite or reads any production bearer.
    validate(live, candidate, root)
    verify_installed_units(live, units)

    status = {unit: _state(unit) for unit in (*OBSERVATIONS, *UNITS)}
    if any(status[name] != "ACTIVE" for name in OBSERVATIONS):
        raise ShadowRefused("OBSERVATION_SERVICE_MUST_REMAIN_ACTIVE")

    if any(status[name] != "ACTIVE" for name in (BACKEND, TIMER)):
        raise ShadowRefused("EXISTING_SHARED_SERVICE_NOT_ACTIVE")
    if status[MIRROR] != "INACTIVE":
        raise ShadowRefused("PC_MIRROR_ONESHOT_BUSY")

    # One-time status is not a quiescence guarantee. In particular the active
    # timer can start the mirror at any moment after this inspection.
    return {
        "S6M_PINNED_UNITS_AND_SHADOW": "VERIFIED",
        "OBSERVATION_SERVICES": "ACTIVE_UNTOUCHED",
        "SHARED_BACKEND": "ACTIVE",
        "SHARED_TIMER": "ACTIVE",
        "PC_MIRROR_ONESHOT": "INACTIVE_AT_INSPECTION_ONLY",
        "COLD_CHECKPOINT_READY": "NO_ACTIVE_WRITERS_NOT_FROZEN",
        "OPERATOR_WINDOW_REQUIRED": "YES",
        "CROSS_DB_ATOMICITY": "NOT_PROVEN",
        "PHONE_TLS_GATE": "NOT_VERIFIED",
        "LIVE_SERVICE_CHANGE": "NOT_ATTEMPTED",
        "PRODUCTION_APPROVAL": "NO",
    }


def main() -> int:
    home = Path.home()
    live = home / "ORUN-SHARED-RUNTIME"
    candidate = home / "ORUN-ACCOUNT-CANDIDATE-S6I"
    units = home / ".config" / "systemd" / "user"
    print("=== ORUN S6M COLD CUTOVER READ-ONLY PREFLIGHT ===")
    try:
        for k, v in inspect(live, candidate, units).items():
            print(k + "=" + v)
    except (ShadowRefused, OSError, ValueError) as error:
        why = str(error) if isinstance(error, ShadowRefused) else "READ_ONLY_PREFLIGHT_ERROR"
        print("ORUN_S6M_PREFLIGHT_REFUSED=" + why, file=sys.stderr)
        return 2
    print("ORUN_S6M_READ_ONLY_PREFLIGHT_PASS")
    print("IMPORTANT=NO_STOP_NO_COLD_BACKUP_NO_ACCOUNT_ACTIVATION")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
