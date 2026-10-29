#!/usr/bin/env python3
"""ORUN S4C — standalone *loopback-only* authenticated shared-data backend.

Unlike the earlier test harness, this is an explicit CLI process that can
be started, stopped and restarted against a separate owner-controlled state
directory. It is NOT the running ORUN observation backend and MUST NOT be
bound to a public/Tailscale interface without a future audited TLS boundary.

No enrollment/approval endpoint is provided. The owner approval methods in
backend.edge_enrollment remain trusted local operations, not public HTTP.
No firmware, gateway, phone DB or PC Edge observation service is changed.
"""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import re
import stat
import sys
from http.server import ThreadingHTTPServer

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from backend.edge_enrollment import EdgeEnrollment  # noqa: E402
from backend.shared_sync_http import handler_factory  # noqa: E402
from backend.phone_enrollment_http import mobile_handler_factory  # noqa: E402
from backend.shared_sync_store import SharedStore  # noqa: E402

FILES = ("shared_resources.sqlite", "enrolled_edges.sqlite")
CONTROL = re.compile(r"^127\.0\.0\.1$")


class StartupRefused(RuntimeError):
    pass


def _check_dir(directory: Path) -> None:
    if directory.is_symlink() or not directory.is_dir():
        raise StartupRefused("PRIVATE_STATE_DIRECTORY_REQUIRED")
    s = directory.stat()
    if s.st_uid != os.geteuid() or stat.S_IMODE(s.st_mode) & 0o077:
        raise StartupRefused("PRIVATE_STATE_DIRECTORY_PERMISSIONS")
    if directory.resolve() in (Path.home(), ROOT, ROOT / "backend"):
        raise StartupRefused("DEDICATED_DIRECTORY_REQUIRED")


def _ensure_private_db(path: Path, reuse_state: bool) -> None:
    if path.is_symlink():
        raise StartupRefused("PRIVATE_DB_SYMLINK_DENIED")
    if path.exists():
        st = path.stat()
        if not reuse_state:
            raise StartupRefused("STATE_EXISTS_REQUIRES_EXPLICIT_REUSE")
        if (not stat.S_ISREG(st.st_mode) or st.st_uid != os.geteuid() or
                stat.S_IMODE(st.st_mode) & 0o077):
            raise StartupRefused("PRIVATE_DB_PERMISSIONS")
        return
    if reuse_state:
        raise StartupRefused("STATE_REUSE_REQUIRES_EXISTING_PAIR")
    flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL | getattr(os, "O_NOFOLLOW", 0)
    try:
        fd = os.open(path, flags, 0o600)
    except FileExistsError as exc:
        raise StartupRefused("STATE_CREATION_RACE") from exc
    else:
        os.close(fd)


def prepare(state_dir: str | Path, *, reuse_state: bool = False) -> tuple[SharedStore, EdgeEnrollment]:
    directory = Path(state_dir)
    _check_dir(directory)
    paths = [directory / name for name in FILES]
    # A fresh pair is created only when BOTH paths are absent; never
    # overwrite or opportunistically adopt a partial/foreign database.
    exists = [p.exists() or p.is_symlink() for p in paths]
    if any(exists) != all(exists):
        raise StartupRefused("PARTIAL_STATE_PAIR_REFUSED")
    if any(exists) != reuse_state:
        raise StartupRefused("STATE_MODE_MISMATCH")
    for p in paths:
        _ensure_private_db(p, reuse_state)
    return SharedStore(paths[0]), EdgeEnrollment(paths[1])


def start(state_dir: str | Path, bind: str, port: int,
          *, reuse_state: bool = False, phone_enrollment: bool = False) -> None:
    if not CONTROL.fullmatch(bind):
        raise StartupRefused("ONLY_127_0_0_1_BIND_ALLOWED")
    if type(port) is not int or not 0 <= port <= 65535:
        raise StartupRefused("INVALID_PORT")
    store, enrollment = prepare(state_dir, reuse_state=reuse_state)
    # Phone registration stays OFF unless an operator explicitly enables it.
    handler = (mobile_handler_factory(store, enrollment) if phone_enrollment
               else handler_factory(store, enrollment.resolve_headers))
    server = ThreadingHTTPServer(("127.0.0.1", port), handler)
    server.daemon_threads = True
    try:
        host, assigned = server.server_address[:2]
        if host != "127.0.0.1":
            raise StartupRefused("UNEXPECTED_PUBLIC_BIND")
        print(f"ORUN_SHARED_BIND={host}:{assigned}", flush=True)
        print("ORUN_SHARED_SECURITY=STEP3C_ENROLLED_ONLY;LOOPBACK_HTTP_ONLY", flush=True)
        server.serve_forever(poll_interval=0.2)
    finally:
        server.server_close()


def main() -> int:
    parser = argparse.ArgumentParser(
        description="ORUN isolated authenticated shared sync: loopback-only")
    parser.add_argument("--state-dir", required=True,
                        help="Separate pre-existing owner-only 0700 directory")
    parser.add_argument("--bind", default="127.0.0.1",
                        help="Only literal 127.0.0.1 accepted")
    parser.add_argument("--port", type=int, default=0,
                        help="Loopback port, 0 for an ephemeral host-test port")
    parser.add_argument("--reuse-state", action="store_true",
                        help="Explicitly reopen both existing private SQLite files")
    parser.add_argument("--enable-phone-enrollment", action="store_true",
                        help="opt in to untrusted begin/finish; owner approves ONLY locally")
    args = parser.parse_args()
    try:
        start(args.state_dir, args.bind, args.port, reuse_state=args.reuse_state,
              phone_enrollment=args.enable_phone_enrollment)
        return 0
    except (StartupRefused, OSError, ValueError) as exc:
        reason = str(exc) if isinstance(exc, StartupRefused) else "RUNTIME_ERROR"
        print("ORUN_SHARED_START_REFUSED=" + reason, file=sys.stderr)
        return 2
    except KeyboardInterrupt:
        return 0


if __name__ == "__main__":
    raise SystemExit(main())
