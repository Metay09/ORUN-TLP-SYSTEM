#!/usr/bin/env python3
"""ORUN S6N: operator-gated cold backup of the S4E shared sidecar.

Default mode is a read-only plan. Execution requires BOTH --execute and an
exact maintenance acknowledgement. Execution pauses ONLY the shared sidecar
timer/backend, while observation backend and PC Edge remain running.
No account activation, systemd unit replacement, token rotation or restore.

This backs up DBs from one known-writer quiescence window, including mirror
cache, bearer file and pinned S4E source. It is NOT proof that some unknown
external process cannot write these files, nor a successful live cutover.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import signal
import sqlite3
import stat
import sys
import time

ROOT = Path(__file__).resolve().parents[2]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from tools.ci.preflight_account_cold_cutover_s6m import (
    BACKEND, MIRROR, TIMER, OBSERVATIONS, UNITS,
    _systemctl, _state, inspect,
)
from tools.ci.probe_s4e_shadow_rollback_s6k import LEGACY_SOURCE
from tools.ci.shadow_account_sqlite_migration_s6j import (
    ShadowRefused, backup_sqlite, check_db, digest, private_path,
)

ACKNOWLEDGEMENT = "I_APPROVE_ORUN_SHARED_SIDECAR_PAUSE"
OUTPUT = "COLD_S6N"
DB_PATHS = (
    ("state/backend/shared_resources.sqlite", "sqlite"),
    ("state/backend/enrolled_edges.sqlite", "sqlite"),
    ("state/pc/shared.sqlite", "sqlite"),
)
OTHER_PATHS = (
    "state/secrets/pc-enrollment.token",
    "STAGING_MANIFEST.json",
    *(f"runtime/{relative}" for relative in LEGACY_SOURCE),
)
UNITS_OUTPUT = "installed_units"


def _systemd(action: str, unit: str) -> None:
    if (action not in ("start", "stop") or
            unit not in (BACKEND, TIMER)):
        raise ShadowRefused("UNAUTHORIZED_SERVICE_ACTION")
    result = _systemctl(action, unit)
    if result.returncode:
        raise ShadowRefused("SYSTEMD_" + action.upper() + "_FAILED_" +
                            unit.replace(".", "_").replace("-", "_"))


def _copy_private(source: Path, target: Path) -> None:
    # O_NOFOLLOW + fd-level fstat prevent a symlink or public-readable token
    # from being copied through an unsafe race.
    private_path(source, directory=False)
    fd = os.open(source, os.O_RDONLY | getattr(os, "O_NOFOLLOW", 0))
    try:
        info = os.fstat(fd)
        if (not stat.S_ISREG(info.st_mode) or
                info.st_uid != os.geteuid() or
                stat.S_IMODE(info.st_mode) & 0o077):
            raise ShadowRefused("COPY_SOURCE_NOT_PRIVATE")
        out = os.open(target, os.O_WRONLY | os.O_CREAT | os.O_EXCL |
                      getattr(os, "O_NOFOLLOW", 0), 0o600)
        try:
            while True:
                data = os.read(fd, 1024 * 1024)
                if not data:
                    break
                view = memoryview(data)
                while view:
                    view = view[os.write(out, view):]
            os.fsync(out)
        finally:
            os.close(out)
    finally:
        os.close(fd)
    private_path(target, directory=False)


def _sqlite_backup(source: Path, target: Path) -> None:
    private_path(source, directory=False)
    backup_sqlite(source, target)
    check_db(target)


def _copy_snapshot(live: Path, units: Path, output: Path) -> dict:
    items: dict[str, str] = {}
    for relative, kind in DB_PATHS:
        original = live / relative
        destination = output / relative
        destination.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
        if kind != "sqlite":
            raise ShadowRefused("INVALID_BACKUP_KIND")
        _sqlite_backup(original, destination)
        items[relative] = digest(destination)

    for relative in OTHER_PATHS:
        original = live / relative
        destination = output / relative
        destination.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
        _copy_private(original, destination)
        items[relative] = digest(destination)

    for unit in UNITS:
        relative = f"{UNITS_OUTPUT}/{unit}"
        destination = output / relative
        destination.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
        _copy_private(units / unit, destination)
        items[relative] = digest(destination)

    # Exact snapshot contents validated before declaration of completion.
    for relative, checksum in items.items():
        target = output / relative
        if digest(target) != checksum:
            raise ShadowRefused("SNAPSHOT_HASH_MISMATCH")
        private_path(target, directory=False)
    return {
        "schema": 1,
        "state": "SERVICE_PAUSED_BACKUP_NOT_ACTIVATED",
        "site": "orun-main",
        "installation": "pc-main",
        "source_commit": "264b4117a4d9fdda2b3ed8f328d0e02f17ee52bc",
        "files_sha256": items,
        "sqlite_files": [p for p, _ in DB_PATHS],
        "quiesce_scope": [TIMER, BACKEND, MIRROR],
        "observation_services": "KEPT_ACTIVE",
        "unknown_external_writers": "NOT_PROVEN_ABSENT",
        "production_restore_test": "NOT_YET_PERFORMED",
        "account_login": "NOT_ENABLED",
    }


def _wait_mirror_inactive() -> None:
    for _ in range(30):
        if _state(MIRROR) == "INACTIVE":
            return
        time.sleep(0.5)
    raise ShadowRefused("PC_MIRROR_BUSY_CANNOT_QUIESCE")


def _interrupted(_signum: int, _frame: object) -> None:
    raise ShadowRefused("MAINTENANCE_INTERRUPTED")


def cold_checkpoint(live: Path, candidate: Path, units: Path, *,
                    execute: bool = False,
                    acknowledgement: str | None = None,
                    root: Path = ROOT) -> dict | None:
    inspect(live, candidate, units, root=root)
    checkpoint = candidate / OUTPUT
    if checkpoint.exists() or checkpoint.is_symlink():
        raise ShadowRefused("COLD_CHECKPOINT_ALREADY_EXISTS")
    if not execute:
        print("ORUN_S6N_PLAN_READ_ONLY_PASS")
        print("OBSERVATION_BACKEND_AND_PC_EDGE_WILL_REMAIN_ACTIVE=YES")
        print("SERVICE_PAUSE_REQUIRED=SHARED_TIMER_AND_SHARED_BACKEND_ONLY")
        print("BACKUP_CREATED=NO")
        print("PRODUCTION_APPROVAL=NO")
        return None

    if acknowledgement != ACKNOWLEDGEMENT:
        raise ShadowRefused("EXPLICIT_MAINTENANCE_APPROVAL_REQUIRED")

    # Prevent accidental service mutation from code outside the user-owned
    # repository and ownership/permissions checked by S6M.
    private_path(candidate, directory=True)
    old_sigterm = signal.getsignal(signal.SIGTERM)
    old_sigint = signal.getsignal(signal.SIGINT)
    signal.signal(signal.SIGTERM, _interrupted)
    signal.signal(signal.SIGINT, _interrupted)

    timer_attempted = False
    backend_attempted = False
    created = False
    finished = False
    resume_failed = False
    data: dict | None = None
    try:
        # Timer first, so it cannot schedule a fresh mirror run while the
        # shared backend is stopped. A running oneshot must finish naturally.
        timer_attempted = True
        _systemd("stop", TIMER)
        if _state(TIMER) != "INACTIVE":
            raise ShadowRefused("TIMER_DID_NOT_STOP")
        _wait_mirror_inactive()

        backend_attempted = True
        _systemd("stop", BACKEND)
        if _state(BACKEND) != "INACTIVE":
            raise ShadowRefused("SHARED_BACKEND_DID_NOT_STOP")
        if any(_state(s) != "ACTIVE" for s in OBSERVATIONS):
            raise ShadowRefused("OBSERVATION_SERVICE_NOT_ACTIVE")
        if _state(MIRROR) != "INACTIVE":
            raise ShadowRefused("MIRROR_BECAME_ACTIVE")

        checkpoint.mkdir(mode=0o700)
        created = True
        payload = checkpoint / "bundle"
        payload.mkdir(mode=0o700)
        data = _copy_snapshot(live, units, payload)
        # All known writers must remain stopped through the entire copy.
        if (_state(TIMER) != "INACTIVE" or
                _state(BACKEND) != "INACTIVE" or
                _state(MIRROR) != "INACTIVE"):
            raise ShadowRefused("QUIESCENCE_LOST_DURING_BACKUP")
        manifest = checkpoint / "CHECKPOINT_MANIFEST.json"
        raw = (json.dumps(data, indent=2, sort_keys=True) + "\n").encode()
        descriptor = os.open(manifest, os.O_WRONLY | os.O_CREAT |
                             os.O_EXCL | getattr(os, "O_NOFOLLOW", 0), 0o600)
        try:
            os.write(descriptor, raw)
            os.fsync(descriptor)
        finally:
            os.close(descriptor)
        finished = True
    finally:
        # Even on backup failure try to restore the OLD system. Never change
        # observation backend or PC Edge; no new account server is launched.
        # Restart backend before timer, and never claim success if either fails.
        if backend_attempted:
            try:
                _systemd("start", BACKEND)
            except ShadowRefused:
                resume_failed = True
        if timer_attempted:
            try:
                _systemd("start", TIMER)
            except ShadowRefused:
                resume_failed = True
        try:
            if (backend_attempted and _state(BACKEND) != "ACTIVE" or
                    timer_attempted and _state(TIMER) != "ACTIVE" or
                    any(_state(s) != "ACTIVE" for s in OBSERVATIONS)):
                resume_failed = True
        except ShadowRefused:
            resume_failed = True
        if created and not finished:
            shutil.rmtree(checkpoint)
        signal.signal(signal.SIGTERM, old_sigterm)
        signal.signal(signal.SIGINT, old_sigint)
        if resume_failed:
            # Preserve completed backup, if any, for owner inspection.
            raise ShadowRefused("CRITICAL_OLD_SERVICE_RESTART_REVIEW_REQUIRED")

    if not finished or data is None:
        raise ShadowRefused("CHECKPOINT_NOT_FINISHED")
    print("ORUN_S6N_COLD_CHECKPOINT_PASS")
    print("BACKUP_FILES=" + str(len(data["files_sha256"])))
    print("OLD_SHARED_BACKEND_RESTARTED=YES")
    print("OLD_SHARED_MIRROR_TIMER_RESTARTED=YES")
    print("OBSERVATION_SERVICES_UNTOUCHED=YES")
    print("NEW_ACCOUNT_BACKEND_ACTIVATED=NO")
    print("PRODUCTION_ROLLBACK_RESTORE_TEST=NOT_YET_PERFORMED")
    return data


def main() -> int:
    parser = argparse.ArgumentParser(
        description="ORUN S6N cold checkpoint (plan only unless explicitly approved)")
    parser.add_argument("--execute", action="store_true",
                        help="Pauses shared sidecar ONLY with explicit consent")
    parser.add_argument("--acknowledgement", default=None,
                        help="Exact maintenance approval phrase; no default")
    a = parser.parse_args()
    home = Path.home()
    try:
        cold_checkpoint(
            home / "ORUN-SHARED-RUNTIME",
            home / "ORUN-ACCOUNT-CANDIDATE-S6I",
            home / ".config" / "systemd" / "user",
            execute=a.execute, acknowledgement=a.acknowledgement)
        return 0
    except (ShadowRefused, OSError, sqlite3.Error) as error:
        reason = str(error) if isinstance(error, ShadowRefused) else "CHECKPOINT_ERROR"
        print("ORUN_S6N_REFUSED=" + reason, file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
