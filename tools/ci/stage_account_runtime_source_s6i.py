#!/usr/bin/env python3
"""S6I: stage a *source-only*, private account-sidecar candidate.

Nothing is activated. No live state, credential, token, DB, systemd unit,
Tailscale setting or APK is read/modified. Source contents are pinned by SHA256.
The candidate is deliberately NOT a deployable/activated runtime by itself.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import stat
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
SOURCES = (
    "backend/shared_sync_store.py",
    "backend/shared_sync_http.py",
    "backend/edge_enrollment.py",
    "backend/phone_enrollment_http.py",
    "backend/account_login.py",
    "backend/account_login_http.py",
    "tools/orun_shared_sync_loopback.py",
    "tools/orun_shared_pc_mirror.py",
    "tools/orun_shared_local_pc_enroll.py",
    "tools/orun_account_bootstrap.py",
)
PROTECTED_HOME = (
    "ORUN-SHARED-RUNTIME",
    "ORUN-SHARED-VERIFY.SxcWay",
    "ORUN-TLP-SYSTEM",
    "ORUN-PC-EDGE-VERIFY",
)

class StagingRefused(ValueError):
    pass

def _check_destination(destination: Path, home: Path) -> None:
    if not destination.is_absolute():
        raise StagingRefused("ABSOLUTE_DESTINATION_REQUIRED")
    if destination.exists() or destination.is_symlink():
        raise StagingRefused("DESTINATION_ALREADY_EXISTS")
    parent = destination.parent
    if parent.is_symlink() or not parent.is_dir():
        raise StagingRefused("DIRECT_PARENT_REQUIRED")
    meta = parent.lstat()
    if meta.st_uid != os.geteuid() or not stat.S_ISDIR(meta.st_mode):
        raise StagingRefused("PARENT_MUST_BE_OWNED")
    resolved = destination.resolve(strict=False)
    protected = [ROOT.resolve(), home / ".config" / "systemd",
                 *(home / item for item in PROTECTED_HOME)]
    if (resolved == home or resolved == home / ".config" or
            any(resolved == p or resolved.is_relative_to(p)
                for p in protected)):
        raise StagingRefused("PROTECTED_LOCATION")

def _write_new(path: Path, content: bytes) -> None:
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL |
                 getattr(os, "O_NOFOLLOW", 0), 0o600)
    with os.fdopen(fd, "wb") as stream:
        stream.write(content)
        stream.flush()
        os.fsync(stream.fileno())

def stage(destination: Path, *, root: Path = ROOT,
          home: Path | None = None) -> dict:
    home = Path.home() if home is None else home
    _check_destination(destination, home)
    meta: dict[str, str] = {}
    source_data: dict[str, bytes] = {}
    for rel in SOURCES:
        original = root / rel
        st = original.lstat()
        if not stat.S_ISREG(st.st_mode) or st.st_uid != os.geteuid():
            raise StagingRefused("SOURCE_UNSAFE_" + rel.replace("/", "_"))
        binary = original.read_bytes()
        if not binary:
            raise StagingRefused("EMPTY_SOURCE")
        source_data[rel] = binary
        meta[rel] = hashlib.sha256(binary).hexdigest()

    try:
        source_commit = subprocess.check_output(
            ["git", "-C", str(root), "rev-parse", "HEAD"],
            text=True, stderr=subprocess.DEVNULL, timeout=5,
        ).strip()
    except (OSError, subprocess.SubprocessError):
        raise StagingRefused("GIT_COMMIT_UNAVAILABLE") from None
    if not (len(source_commit) == 40 and
            all(ch in "0123456789abcdef" for ch in source_commit)):
        raise StagingRefused("INVALID_COMMIT")

    manifest = {
        "schema": 1,
        "state": "SOURCE_ONLY_NOT_INSTALLED",
        "source_commit": source_commit,
        "sha256": meta,
        "site": "NOT_PROVISIONED",
        "account_mode": "NOT_ENABLED",
        "phone_enrollment": "NOT_ENABLED",
        "contains_private_data": False,
        "systemd": "NOT_INSTALLED",
        "runtime_e2e": "NOT_YET_VALIDATED",
    }

    previous_umask = os.umask(0o077)
    created = False
    try:
        destination.mkdir(mode=0o700)
        created = True
        runtime = destination / "runtime"
        runtime.mkdir(mode=0o700)
        for group in ("backend", "tools"):
            (runtime / group).mkdir(mode=0o700)
        for rel, binary in source_data.items():
            _write_new(runtime / rel, binary)
        _write_new(destination / "SOURCE_MANIFEST.json",
                   (json.dumps(manifest, sort_keys=True, indent=2) + "\n").encode())
        return manifest
    except BaseException:
        if created:
            shutil.rmtree(destination)
        raise
    finally:
        os.umask(previous_umask)

def main() -> int:
    parser = argparse.ArgumentParser(description="ORUN S6I source-only staging")
    parser.add_argument("--dest", required=True,
                        help="New, absolute, owned destination; never a live runtime")
    args = parser.parse_args()
    try:
        manifest = stage(Path(args.dest))
    except (StagingRefused, OSError) as error:
        reason = str(error) if isinstance(error, StagingRefused) else "FILESYSTEM_ERROR"
        print("ORUN_S6I_STAGE_REFUSED=" + reason, file=sys.stderr)
        return 2
    print("ORUN_S6I_SOURCE_ONLY_STAGE_PASS")
    print("SOURCE_COMMIT=" + manifest["source_commit"])
    print("SOURCE_FILES=" + str(len(manifest["sha256"])))
    print("ACCOUNT_MODE=NOT_ENABLED")
    print("LIVE_SERVICES_CHANGED=NO")
    print("READY_FOR_DEPLOYMENT=NO")
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
