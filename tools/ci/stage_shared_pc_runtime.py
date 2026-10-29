#!/usr/bin/env python3
"""ORUN S4E: stage a PINNED, owner-only PC shared-sync sidecar.

This command creates a new, isolated directory containing a private code
snapshot, separate SharedStore/EdgeEnrollment/PC-mirror SQLite state, a
read-only per-PC Step3C credential, and *disabled* user-systemd unit files.

It NEVER installs, starts, stops or reloads ANY systemd service. In
particular it NEVER touches orun-backend.service, orun-pc-edge.service,
the existing /v1/edge/batch observation path or Android app data.

The staged HTTP service listens only on 127.0.0.1; remote HTTPS and
real phone enrollment are separate future gates. Credentials expire in
30 days and MUST NOT be used as production unattended authentication.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import socket
import stat
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from tools.orun_shared_pc_mirror import ID, PcSharedMirror  # noqa: E402
from tools.orun_shared_sync_loopback import prepare  # noqa: E402
from tools.orun_shared_local_pc_enroll import authorize_local_pc  # noqa: E402
from tools.ci.render_shared_pc_timer import render, _absolute  # noqa: E402

# Exact import closure, copied as an immutable-by-operator source snapshot.
SOURCES = (
    "backend/shared_sync_store.py",
    "backend/shared_sync_http.py",
    "backend/edge_enrollment.py",
    "tools/orun_shared_pc_mirror.py",
    "tools/orun_shared_sync_loopback.py",
    "tools/orun_shared_local_pc_enroll.py",
)
SERVICE = "orun-shared-sync.service"


class StageRefused(RuntimeError):
    pass


def _owner_directory(path: Path) -> None:
    if path.is_symlink() or not path.is_dir():
        raise StageRefused("STAGING_PARENT_INVALID")
    st = path.stat()
    if st.st_uid != os.geteuid():
        raise StageRefused("STAGING_PARENT_NOT_OWNED")


def _port_available(port: int) -> None:
    if type(port) is not int or not 1024 <= port <= 65535:
        raise StageRefused("STAGING_PORT_INVALID")
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        try:
            s.bind(("127.0.0.1", port))
        except OSError as exc:
            raise StageRefused("STAGING_PORT_OCCUPIED") from exc


def _write_private_new(path: Path, content: bytes) -> None:
    flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL | getattr(os, "O_NOFOLLOW", 0)
    fd = os.open(path, flags, 0o600)
    with os.fdopen(fd, "wb") as f:
        f.write(content)
        f.flush()
        os.fsync(f.fileno())


def stage(destination: Path, site: str, installation: str,
          *, approved: bool, port: int = 18781) -> dict:
    if not approved:
        raise StageRefused("EXPLICIT_LOCAL_PC_APPROVAL_REQUIRED")
    if (not isinstance(site, str) or not ID.fullmatch(site) or
            not isinstance(installation, str) or
            not installation.startswith("pc-") or
            not ID.fullmatch(installation)):
        raise StageRefused("STAGING_IDENTITY_INVALID")
    destination = Path(destination)
    if not destination.is_absolute() or destination.exists() or destination.is_symlink():
        raise StageRefused("STAGING_NEW_ABSOLUTE_DIRECTORY_REQUIRED")
    if destination.resolve().is_relative_to(ROOT.resolve()) or (
            destination.resolve().is_relative_to(
                Path.home() / "ORUN-PC-EDGE-VERIFY")):
        raise StageRefused("STAGING_MUST_NOT_USE_EXISTING_ORUN_CODE")
    _owner_directory(destination.parent)
    _port_available(port)

    old_umask = os.umask(0o077)
    created = False
    try:
        destination.mkdir(mode=0o700)
        created = True
        for name in ("runtime", "runtime/backend", "runtime/tools",
                     "state", "state/backend", "state/pc",
                     "state/secrets", "units"):
            (destination / name).mkdir(mode=0o700)

        manifest = {}
        for rel in SOURCES:
            origin = ROOT / rel
            if (origin.is_symlink() or not origin.is_file() or
                    not stat.S_ISREG(origin.stat().st_mode)):
                raise StageRefused("STAGING_SOURCE_INVALID")
            binary = origin.read_bytes()
            _write_private_new(destination / "runtime" / rel, binary)
            manifest[rel] = hashlib.sha256(binary).hexdigest()

        backend_state = destination / "state" / "backend"
        secrets = destination / "state" / "secrets"
        token = secrets / "pc-enrollment.token"
        mirror_file = destination / "state" / "pc" / "shared.sqlite"
        runtime = destination / "runtime"
        origin = f"http://127.0.0.1:{port}"

        # New independent DB pair; never the running observation Backend.
        prepare(backend_state, reuse_state=False)
        fingerprint = authorize_local_pc(
            backend_state, site=site, actor="owner", installation=installation,
            token_out=token, explicitly_approved=True)
        # New independent read mirror, bound to exact site and installation.
        mirror = PcSharedMirror(mirror_file, site, installation, origin)
        if mirror.snapshot()["cursor"] != 0:
            raise StageRefused("STAGING_NEW_MIRROR_NOT_EMPTY")

        units_dir = destination / "units"
        mirror_svc, mirror_timer = render(runtime, units_dir, mirror_file,
                                          site, installation, origin, token,
                                          Path(sys.executable))
        # An explicit dependency ensures a timer firing after reboot can
        # bring up its ONLY related shared-data server, not the live bench.
        unit = mirror_svc.read_text(encoding="utf-8")
        if "Wants=network-online.target" not in unit:
            raise StageRefused("STAGING_MIRROR_UNIT_UNEXPECTED")
        unit = unit.replace("Wants=network-online.target",
                            "Requires=orun-shared-sync.service\nAfter=orun-shared-sync.service")
        # Preserve 0600; this is a new isolated staging file only.
        mirror_svc.write_text(unit, encoding="utf-8")

        interpreter = _absolute(sys.executable)
        server_script = _absolute(str(runtime / "tools" / "orun_shared_sync_loopback.py"))
        db_dir = _absolute(str(backend_state))
        server_unit = f"""# STAGED ONLY. DO NOT INSTALL UNTIL OPERATOR HAS APPROVED DEPLOYMENT.
[Unit]
Description=ORUN shared-data Backend (independent, loopback-only)
After=network.target

[Service]
Type=simple
UMask=0077
NoNewPrivileges=true
PrivateTmp=true
Restart=on-failure
RestartSec=10s
ExecStart={interpreter} {server_script} --state-dir {db_dir} --bind 127.0.0.1 --port {port} --reuse-state

[Install]
WantedBy=default.target
"""
        _write_private_new(units_dir / SERVICE, server_unit.encode("utf-8"))
        for name in (SERVICE, mirror_svc.name, mirror_timer.name):
            text = (units_dir / name).read_text(encoding="utf-8")
            if not text or "ORUN" not in text or "8090" in text:
                raise StageRefused("STAGING_UNIT_INVALID")

        try:
            version = subprocess.check_output(
                ["git", "-C", str(ROOT), "rev-parse", "HEAD"],
                text=True, stderr=subprocess.DEVNULL, timeout=4).strip()
        except (OSError, subprocess.SubprocessError):
            version = "unknown"
        details = {
            "source_commit": version, "source_sha256": manifest,
            "site": site, "installation": installation, "loopback_port": port,
            "pc_public_fingerprint": fingerprint, "unit_names":
                [SERVICE, mirror_svc.name, mirror_timer.name],
            "runtime_state": "STAGED_NOT_INSTALLED",
            "access": "OWNER_ONLY_0600",
            "next_gate": "REVIEW_UNITS_AND_APPROVE_EXPLICIT_ACTIVATION",
            "token_expires": "30_DAYS_FROM_ENROLLMENT_NO_AUTO_RENEWAL",
        }
        _write_private_new(destination / "STAGING_MANIFEST.json",
                           (json.dumps(details, indent=2, ensure_ascii=False) + "\n").encode())
        return details
    except BaseException:
        # Only a destination newly created by this invocation may be removed.
        if created:
            shutil.rmtree(destination)
        raise
    finally:
        os.umask(old_umask)


def main() -> int:
    p = argparse.ArgumentParser(
        description="Stage a *disabled* independent ORUN shared sync runtime")
    p.add_argument("--dest", default=str(Path.home() / "ORUN-SHARED-RUNTIME"),
                   help="brand new, separate owner-only directory")
    p.add_argument("--site", required=True)
    p.add_argument("--installation", default="pc-main")
    p.add_argument("--port", type=int, default=18781)
    p.add_argument("--approve-local-pc", action="store_true",
                   help="explicit owner permission for a read-only local PC identity")
    a = p.parse_args()
    try:
        result = stage(Path(a.dest), a.site, a.installation,
                       approved=a.approve_local_pc, port=a.port)
    except Exception as exc:
        code = str(exc) if isinstance(exc, StageRefused) else type(exc).__name__
        print("ORUN_STAGING_REFUSED=" + code, file=sys.stderr)
        return 2
    print("ORUN_SHARED_SIDECAR_STAGED=YES")
    print("STAGED_DIR=" + a.dest)
    print("STAGED_SOURCE_COMMIT=" + result["source_commit"][:12])
    print("PORT=127.0.0.1:" + str(result["loopback_port"]))
    print("OWNER_PC_READ_ONLY_ENROLLED=YES")
    print("UNIT_FILES_STAGED_NOT_INSTALLED=" + ",".join(result["unit_names"]))
    print("NO_SYSTEMD_CHANGES;NO_LIVE_DB_CHANGE;NO_ANDROID_FIRMWARE_CHANGE")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
