#!/usr/bin/env python3
"""ORUN S4E: explicit opt-in, reversible activation of a PINNED local sidecar.

ONLY installs: orun-shared-sync.service, orun-shared-pc-mirror.service and
orun-shared-pc-mirror.timer from an already staged owner-private directory.
Never modifies, restarts or reads the observation Backend/PC Edge databases,
Gateway, Tracker, Android, or 8090 service.

New services are loopback-only. This is a local pilot activation, NOT mobile
deployment, remote enrollment, TLS provisioning or durable token rotation.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import socket
import stat
import subprocess
import sys
import time
from urllib import error, request

ROOT = Path(__file__).resolve().parents[2]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from tools.ci.stage_shared_pc_runtime import SOURCES, SERVICE as BACKEND_UNIT
from tools.ci.render_shared_pc_timer import UNIT as MIRROR_UNIT, TIMER

UNITS = (BACKEND_UNIT, MIRROR_UNIT, TIMER)
OBSERVATIONS = ("orun-backend.service", "orun-pc-edge.service")


class ActivationRefused(RuntimeError):
    pass


def run(*args: str) -> str:
    result = subprocess.run(["systemctl", "--user", *args],
                            capture_output=True, text=True, timeout=20)
    if result.returncode:
        # Never expose systemd diagnostics; they may include credentials
        # from unrelated user processes in failure messages.
        raise ActivationRefused("SYSTEMD_COMMAND_FAILED_" + args[0].upper())
    return result.stdout.strip()


def private(path: Path, directory: bool) -> None:
    if path.is_symlink() or not path.exists():
        raise ActivationRefused("UNSAFE_STAGED_FILE")
    st = path.stat()
    if st.st_uid != os.geteuid() or stat.S_IMODE(st.st_mode) & 0o077:
        raise ActivationRefused("UNSAFE_STAGED_PERMISSIONS")
    if directory and not stat.S_ISDIR(st.st_mode):
        raise ActivationRefused("EXPECTED_STAGED_DIRECTORY")
    if not directory and not stat.S_ISREG(st.st_mode):
        raise ActivationRefused("EXPECTED_STAGED_FILE")


def check_staging(root: Path, site: str, installation: str) -> tuple[int, Path]:
    if (not root.is_absolute() or root.is_symlink() or
            root.resolve() != root or root == Path.home()):
        raise ActivationRefused("STAGING_ABSOLUTE_DIRECT_DIRECTORY_REQUIRED")
    for path in (root, root / "runtime", root / "runtime/backend",
                 root / "runtime/tools", root / "state",
                 root / "state/backend", root / "state/pc",
                 root / "state/secrets", root / "units"):
        private(path, True)
    manifest = root / "STAGING_MANIFEST.json"
    private(manifest, False)
    data = json.loads(manifest.read_text(encoding="utf-8"))
    if (data.get("runtime_state") != "STAGED_NOT_INSTALLED" or
            data.get("site") != site or data.get("installation") != installation or
            set(data.get("source_sha256", {})) != set(SOURCES)):
        raise ActivationRefused("STAGED_MANIFEST_OR_IDENTITY_MISMATCH")
    port = data.get("loopback_port")
    if type(port) is not int or not 1024 <= port <= 65535 or port == 8090:
        raise ActivationRefused("UNSAFE_LOOPBACK_PORT")
    for rel in SOURCES:
        candidate = root / "runtime" / rel
        private(candidate, False)
        digest = hashlib.sha256(candidate.read_bytes()).hexdigest()
        if digest != data["source_sha256"][rel]:
            raise ActivationRefused("STAGED_SOURCE_CHECKSUM_CHANGED")
    for name in ("shared_resources.sqlite", "enrolled_edges.sqlite"):
        private(root / "state/backend" / name, False)
    private(root / "state/pc/shared.sqlite", False)
    token = root / "state/secrets/pc-enrollment.token"
    private(token, False)
    for name in UNITS:
        unit_path = root / "units" / name
        private(unit_path, False)
        unit = unit_path.read_text(encoding="utf-8")
        if ("ExecStart=" not in unit and name != TIMER) or "8090" in unit:
            raise ActivationRefused("STAGED_UNIT_INVALID")
        if name != TIMER and str(root / "runtime") not in unit:
            raise ActivationRefused("STAGED_RUNTIME_PATH_MISMATCH")
    server = (root / "units" / BACKEND_UNIT).read_text(encoding="utf-8")
    mirror = (root / "units" / MIRROR_UNIT).read_text(encoding="utf-8")
    timer = (root / "units" / TIMER).read_text(encoding="utf-8")
    if (f"--bind 127.0.0.1 --port {port} --reuse-state" not in server or
            str(root / "state/backend") not in server or
            "Requires=orun-shared-sync.service" not in mirror or
            f"http://127.0.0.1:{port}" not in mirror or
            str(token) not in mirror or
            str(root / "state/pc/shared.sqlite") not in mirror or
            str(root / "state/secrets") in server or
            "OnUnitInactiveSec=180s" not in timer):
        raise ActivationRefused("STAGED_UNIT_CONFIGURATION_MISMATCH")
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        try:
            sock.bind(("127.0.0.1", port))
        except OSError as exc:
            raise ActivationRefused("STAGED_PORT_OCCUPIED") from exc
    return port, token


def read_live_pids() -> dict[str, str]:
    pids = {}
    for name in OBSERVATIONS:
        if run("is-active", name) != "active":
            raise ActivationRefused("OBSERVATION_SERVICE_NOT_ACTIVE_" + name)
        pid = run("show", "-p", "MainPID", "--value", name)
        if not pid.isascii() or not pid.isdecimal() or int(pid) < 1:
            raise ActivationRefused("OBSERVATION_PID_INVALID_" + name)
        pids[name] = pid
    return pids


def _exclusive_copy(source: Path, target: Path) -> None:
    fd = os.open(target, os.O_WRONLY | os.O_CREAT | os.O_EXCL |
                 getattr(os, "O_NOFOLLOW", 0), 0o600)
    with os.fdopen(fd, "wb") as out:
        out.write(source.read_bytes())
        out.flush()
        os.fsync(out.fileno())


def identity(port: int, token_path: Path, expected_site: str,
             expected_installation: str, *, wait_seconds: float = 12.0) -> None:
    """Wait until a Type=simple user service actually binds its HTTP port.

    systemctl start/is-active can return before Python imports, SQLite opens,
    and the 127.0.0.1 listener is ready. Only transient connection/timeout
    errors are retried. A real HTTP 401/403 or identity mismatch immediately
    fails closed. Avoid ambient HTTP proxy variables for loopback.
    """
    token = token_path.read_text(encoding="ascii").strip()
    req = request.Request(
        f"http://127.0.0.1:{port}/api/v1/sync/identity",
        headers={"Authorization": "Bearer " + token},
    )
    opener = request.build_opener(request.ProxyHandler({}))
    deadline = time.monotonic() + wait_seconds
    while True:
        try:
            with opener.open(req, timeout=2) as response:
                actual = json.load(response)
                if response.status != 200 or actual != {
                        "site": expected_site, "installation": expected_installation}:
                    raise ActivationRefused("BACKEND_IDENTITY_MISMATCH")
                return
        except error.HTTPError as exc:
            # HTTPError inherits from URLError; an actual denial is NOT
            # a startup race and must never silently pass or be retried.
            raise ActivationRefused("SHARED_BACKEND_HTTP_DENIED_" +
                                    str(exc.code)) from None
        except (error.URLError, TimeoutError, ConnectionError):
            if time.monotonic() >= deadline:
                raise ActivationRefused("SHARED_BACKEND_HTTP_NOT_READY") from None
            time.sleep(min(0.25, max(0.0, deadline - time.monotonic())))


def activate(root: Path, site: str, installation: str, approved: bool) -> None:
    if not approved:
        raise ActivationRefused("EXPLICIT_ACTIVATION_REQUIRED")
    port, token = check_staging(root, site, installation)
    before = read_live_pids()
    units = Path.home() / ".config/systemd/user"
    if units.is_symlink() or not units.is_dir() or units.stat().st_uid != os.geteuid():
        raise ActivationRefused("USER_UNIT_DIRECTORY_NOT_SAFE")
    for name in UNITS:
        if (units / name).exists() or (units / name).is_symlink():
            raise ActivationRefused("SIDE_CAR_UNIT_ALREADY_PRESENT")
        if run("show", "-p", "LoadState", "--value", name) != "not-found":
            raise ActivationRefused("SIDE_CAR_UNIT_ALREADY_LOADED")

    created: list[Path] = []
    activated = False
    try:
        for name in UNITS:
            target = units / name
            _exclusive_copy(root / "units" / name, target)
            created.append(target)
        run("daemon-reload")
        activated = True
        run("enable", "--now", BACKEND_UNIT)
        if run("is-active", BACKEND_UNIT) != "active":
            raise ActivationRefused("SHARED_BACKEND_NOT_RUNNING")
        identity(port, token, site, installation)
        run("start", MIRROR_UNIT)
        if run("show", "-p", "Result", "--value", MIRROR_UNIT) != "success":
            raise ActivationRefused("SHARED_MIRROR_PULL_FAILED")
        run("enable", "--now", TIMER)
        if run("is-active", TIMER) != "active" or (
                run("is-enabled", TIMER) != "enabled"):
            raise ActivationRefused("SHARED_TIMER_NOT_ACTIVE")
        after = read_live_pids()
        if after != before:
            raise ActivationRefused("OBSERVATION_SERVICE_PID_CHANGED")

        print("ORUN_SHARED_SIDECAR_ACTIVATED=YES")
        print("LOOPBACK_BACKEND=127.0.0.1:" + str(port))
        print("PC_MIRROR_READ_ONLY=YES")
        print("MIRROR_TIMER_ENABLED=YES")
        for name in OBSERVATIONS:
            print("ORIGINAL_SERVICE_UNCHANGED=" + name + " PID=" + after[name])
        print("TOKEN_EXPIRY=30_DAYS_AFTER_ENROLLMENT;RENEWAL_NOT_IMPLEMENTED")
        print("PHONE_AND_REMOTE_PC_SYNC=NOT_YET_ENABLED")
    except BaseException:
        # Roll back ONLY this invocation's three new service units, not state.
        if activated:
            for args in (("disable", "--now", TIMER),
                         ("stop", MIRROR_UNIT),
                         ("disable", "--now", BACKEND_UNIT)):
                try:
                    run(*args)
                except Exception:
                    print("ROLLBACK_REVIEW_NEEDED=" + " ".join(args), file=sys.stderr)
        for path in reversed(created):
            try:
                path.unlink()
            except OSError:
                print("ROLLBACK_UNIT_FILE_REVIEW_NEEDED=" + path.name,
                      file=sys.stderr)
        if created:
            try:
                run("daemon-reload")
            except Exception:
                print("ROLLBACK_RELOAD_REVIEW_NEEDED", file=sys.stderr)
        print("STAGED_STATE_RETAINED_FOR_REVIEW=YES", file=sys.stderr)
        raise


def main() -> int:
    p = argparse.ArgumentParser(description="Explicit ORUN local shared sidecar activation")
    p.add_argument("--staged-dir", required=True)
    p.add_argument("--site", required=True)
    p.add_argument("--installation", required=True)
    p.add_argument("--activate", action="store_true",
                   help="explicit consent to install and start ONLY three new user units")
    a = p.parse_args()
    try:
        activate(Path(a.staged_dir), a.site, a.installation, a.activate)
        return 0
    except Exception as exc:
        code = str(exc) if isinstance(exc, ActivationRefused) else type(exc).__name__
        print("ORUN_SHARED_ACTIVATION_REFUSED=" + code, file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
