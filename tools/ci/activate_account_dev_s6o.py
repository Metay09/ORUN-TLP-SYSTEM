#!/usr/bin/env python3
"""ORUN S6O — actual owner-account activation for a DEV PC, not another rehearsal.

Use explicit --activate-dev only on the owner's non-field development PC.
The existing 18781 shared sync retains BOTH old SQLite databases and its PC
Ed25519 bearer; we switch only the shared-sync systemd unit's code path and
enable owner-account/P256 phone enrollment on LOOPBACK (no public HTTP).
Observation backend / PC Edge and mirror timer units are never replaced.

A failed install attempts to restore the *original* shared-sync unit and
restart its server and timer. There is no test-data cleanup and no APK build.
"""
from __future__ import annotations

import argparse
import fcntl
import getpass
import http.client
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import sqlite3
import stat
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[2]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from backend.account_login import AccountError, AccountStore
from tools.ci.preflight_account_cold_cutover_s6m import (
    BACKEND, MIRROR, TIMER, OBSERVATIONS,
    _state, inspect,
)
from tools.ci.shadow_account_sqlite_migration_s6j import (
    PORT, SITE, ShadowRefused, private_path,
)

WORKSPACE = "DEV_INSTALL_S6O"
UNIT_BACKUP = "original-shared-sync.service"
UNIT_NEXT = "account-shared-sync.service"
OWNER_ACTOR = "owner"
USERNAME = "owner"

class DevInstallError(RuntimeError):
    pass


def ctl(*args: str) -> None:
    try:
        result = subprocess.run(
            ["systemctl", "--user", *args], capture_output=True,
            text=True, timeout=25, check=False,
        )
    except (OSError, subprocess.TimeoutExpired):
        raise DevInstallError("SYSTEMD_COMMAND_UNAVAILABLE") from None
    if result.returncode != 0:
        raise DevInstallError("SYSTEMD_" + args[0].upper() + "_FAILED")


def service_state(name: str) -> str:
    try:
        return _state(name)
    except ShadowRefused:
        raise DevInstallError("UNEXPECTED_SERVICE_STATE") from None


def write_exclusive(path: Path, data: bytes) -> None:
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL |
                 getattr(os, "O_NOFOLLOW", 0), 0o600)
    try:
        with os.fdopen(fd, "wb") as f:
            fd = -1
            f.write(data)
            f.flush()
            os.fsync(f.fileno())
    finally:
        if fd >= 0:
            os.close(fd)


def replace_private(path: Path, data: bytes) -> None:
    """Atomically replace ONLY the known, previously verified unit file."""
    private_path(path, directory=False)
    temp = path.with_name(path.name + ".s6o-new")
    write_exclusive(temp, data)
    try:
        os.replace(temp, path)
    finally:
        temp.unlink(missing_ok=True)


def next_unit(original: str, old_script: Path, new_script: Path) -> bytes:
    if not new_script.is_file() or new_script.is_symlink():
        raise DevInstallError("CANDIDATE_SCRIPT_MISSING")
    lines = original.splitlines(keepends=True)
    indices = [i for i, line in enumerate(lines)
               if re.match(r"^\s*ExecStart\s*=", line)]
    if len(indices) != 1:
        raise DevInstallError("EXISTING_UNIT_UNEXPECTED")
    index = indices[0]
    prefix, command = lines[index].split("=", 1)
    tokens = shlex.split(command)
    if tokens.count(str(old_script)) != 1 or "--reuse-state" not in tokens:
        raise DevInstallError("EXISTING_EXECSTART_MISMATCH")
    if ("--enable-account-login" in tokens or
            "--enable-phone-enrollment" in tokens):
        raise DevInstallError("ACCOUNT_MODE_ALREADY_INSTALLED")
    if tokens.count("--port") != 1 or tokens[tokens.index("--port") + 1] != str(PORT):
        raise DevInstallError("PORT_MISMATCH")
    old_index = tokens.index(str(old_script))
    tokens[old_index] = str(new_script)
    tokens += ["--enable-account-login", "--enable-phone-enrollment"]
    # All paths are local absolute paths. No shell interpreter, tokens or
    # password appears in ExecStart. Reject whitespace to avoid systemd
    # escaping/percent expansion surprises in the command.
    if any(not token or any(ch.isspace() or ch in '%"$\\' for ch in token)
           for token in tokens):
        raise DevInstallError("UNSAFE_SYSTEMD_ARGUMENT")
    lines[index] = prefix + "=" + " ".join(tokens) + "\n"
    return "".join(lines).encode("utf-8")


def wait_inactive(unit: str, seconds: float = 20) -> None:
    until = time.monotonic() + seconds
    while time.monotonic() < until:
        if service_state(unit) == "INACTIVE":
            return
        time.sleep(0.25)
    raise DevInstallError("MIRROR_DID_NOT_FINISH")


def validate_owner(path: Path) -> None:
    private_path(path, directory=False)
    with sqlite3.connect("file:" + str(path) + "?mode=ro", uri=True) as db:
        owners = db.execute(
            "SELECT actor_id,username FROM account_users WHERE site_id=?",
            (SITE,),
        ).fetchall()
    if owners != [(OWNER_ACTOR, USERNAME)]:
        raise DevInstallError("OWNER_ACCOUNT_MISMATCH")


def provision_owner(account_path: Path) -> bool:
    if account_path.exists() or account_path.is_symlink():
        validate_owner(account_path)
        print("ORUN_DEV_OWNER_ACCOUNT=ALREADY_PRESENT", flush=True)
        return False

    # This is a development PC, not a signup page. Keep credentials private
    # while letting the operator correct accidental short or mismatched input
    # in this SAME invocation, without retrying Git/fetch/systemd commands.
    print("ORUN hesap kurulumu: 12-128 karakterlik yeni parola gir.", flush=True)
    print("Karakterler terminalde gorunmez; Ctrl+C ile iptal edebilirsin.",
          flush=True)
    while True:
        first = getpass.getpass("Yeni ORUN sahip parolasi: ")
        if not 12 <= len(first) <= 128:
            print("PAROLA KISA/UZUN: En az 12, en fazla 128 karakter olmali. Tekrar dene.",
                  flush=True)
            continue
        again = getpass.getpass("Ayni parolayi ikinci kez gir: ")
        if first != again:
            print("PAROLALAR ESLESMEDI: Her iki girisi yeniden yap.",
                  flush=True)
            continue
        break

    # Password is accepted only through hidden terminal input. It is never
    # copied to argv, environment, systemd, chat or logs.
    try:
        AccountStore(account_path).bootstrap_owner(
            SITE, OWNER_ACTOR, USERNAME, first)
    except (AccountError, ValueError):
        raise DevInstallError("OWNER_BOOTSTRAP_REFUSED") from None
    validate_owner(account_path)
    print("ORUN_DEV_OWNER_ACCOUNT=CREATED", flush=True)
    return True

def check_pc_mirror(token_path: Path) -> None:
    # Read the existing private token only inside the PC; never print or
    # serialize it. Existing mirror continues using its original token.
    private_path(token_path, directory=False)
    token = token_path.read_text(encoding="ascii").strip()
    if not re.fullmatch(r"[A-Za-z0-9_-]{48,128}", token):
        raise DevInstallError("PC_TOKEN_INVALID")
    connection = http.client.HTTPConnection("127.0.0.1", PORT, timeout=3)
    try:
        connection.request("GET", "/api/v1/sync/identity",
                           headers={"Authorization": "Bearer " + token})
        reply = connection.getresponse()
        data = reply.read(16384)
        if reply.status != 200:
            raise DevInstallError("PC_IDENTITY_HTTP_REFUSED")
        body = json.loads(data)
        if body != {"site": SITE, "installation": "pc-main"}:
            raise DevInstallError("PC_IDENTITY_MISMATCH")
    finally:
        connection.close()
    connection = http.client.HTTPConnection("127.0.0.1", PORT, timeout=3)
    try:
        connection.request("GET", "/api/v1/sync?cursor=0",
                           headers={"Authorization": "Bearer " + token})
        reply = connection.getresponse()
        payload = json.loads(reply.read(1024 * 1024))
        if reply.status != 200 or not isinstance(payload.get("changes"), list):
            raise DevInstallError("PC_MIRROR_SYNC_NOT_AUTHORIZED")
    finally:
        connection.close()


def check_account_endpoint() -> None:
    # Do not transmit or log the real password. An unauthenticated account
    # endpoint must not return a successful session or reveal account data.
    conn = http.client.HTTPConnection("127.0.0.1", PORT, timeout=3)
    try:
        conn.request("GET", "/api/v1/account/me")
        response = conn.getresponse()
        response.read(8192)
        if response.status == 404:
            raise DevInstallError("ACCOUNT_ENDPOINT_NOT_MOUNTED")
        if response.status not in (400, 401, 403, 405):
            raise DevInstallError("ACCOUNT_ENDPOINT_UNEXPECTED_RESPONSE")
    finally:
        conn.close()


def wait_healthy(token_path: Path) -> None:
    last = "BACKEND_HEALTH_TIMEOUT"
    until = time.monotonic() + 15
    while time.monotonic() < until:
        if service_state(BACKEND) != "ACTIVE":
            time.sleep(0.3)
            continue
        try:
            check_pc_mirror(token_path)
            check_account_endpoint()
            return
        except (DevInstallError, OSError, ValueError, json.JSONDecodeError):
            last = "BACKEND_HEALTH_TIMEOUT"
        time.sleep(0.3)
    raise DevInstallError(last)


def activate(live: Path, candidate: Path, units: Path) -> None:
    # S6M requires 18781, pinned S4E and the four previously verified
    # services active. No longer requires preserving test records.
    inspect(live, candidate, units, root=ROOT)
    stage = candidate / WORKSPACE
    if stage.exists() or stage.is_symlink():
        raise DevInstallError("DEV_INSTALL_WORKSPACE_EXISTS_INSPECT_FIRST")
    account = live / "state" / "backend" / "owner_accounts.sqlite"
    token_path = live / "state" / "secrets" / "pc-enrollment.token"
    old_script = live / "runtime" / "tools" / "orun_shared_sync_loopback.py"
    new_script = candidate / "runtime" / "tools" / "orun_shared_sync_loopback.py"
    current = units / BACKEND
    private_path(current, directory=False)
    original = current.read_bytes()
    changed = next_unit(original.decode("utf-8"), old_script, new_script)

    # Bootstrap happens before downtime; a cancelled password prompt must
    # not strand a partly created workspace that blocks the next attempt.
    provision_owner(account)

    stage.mkdir(mode=0o700)
    try:
        write_exclusive(stage / UNIT_BACKUP, original)
        write_exclusive(stage / UNIT_NEXT, changed)
    except BaseException:
        (stage / UNIT_BACKUP).unlink(missing_ok=True)
        (stage / UNIT_NEXT).unlink(missing_ok=True)
        stage.rmdir()
        raise

    timer_stopped = False
    backend_stopped = False
    unit_modified = False
    activation_success = False
    critical = False
    try:
        timer_stopped = True
        ctl("stop", TIMER)
        if service_state(TIMER) != "INACTIVE":
            raise DevInstallError("TIMER_DID_NOT_STOP")
        wait_inactive(MIRROR)

        backend_stopped = True
        ctl("stop", BACKEND)
        if service_state(BACKEND) != "INACTIVE":
            raise DevInstallError("OLD_BACKEND_DID_NOT_STOP")
        if any(service_state(s) != "ACTIVE" for s in OBSERVATIONS):
            raise DevInstallError("OBSERVATION_SERVICES_NOT_ACTIVE")

        replace_private(current, changed)
        unit_modified = True
        ctl("daemon-reload")
        ctl("start", BACKEND)
        wait_healthy(token_path)
        ctl("start", TIMER)
        if service_state(TIMER) != "ACTIVE":
            raise DevInstallError("MIRROR_TIMER_DID_NOT_RESTART")
        if any(service_state(s) != "ACTIVE" for s in OBSERVATIONS):
            raise DevInstallError("OBSERVATION_SERVICE_LOST")
        activation_success = True
    finally:
        if not activation_success:
            # Restore original unit and old service, even if launch failed.
            if backend_stopped:
                try:
                    ctl("stop", BACKEND)
                except DevInstallError:
                    critical = True
            if unit_modified:
                try:
                    replace_private(current, original)
                    ctl("daemon-reload")
                except (DevInstallError, OSError, ShadowRefused):
                    critical = True
            if backend_stopped:
                try:
                    ctl("start", BACKEND)
                except DevInstallError:
                    critical = True
            if timer_stopped:
                try:
                    ctl("start", TIMER)
                except DevInstallError:
                    critical = True
            try:
                if (service_state(BACKEND) != "ACTIVE" or
                        service_state(TIMER) != "ACTIVE"):
                    critical = True
            except DevInstallError:
                critical = True
            if critical:
                print("ORUN_S6O_CRITICAL_RECOVERY_REVIEW_REQUIRED=YES",
                      file=sys.stderr, flush=True)
            else:
                print("ORUN_S6O_ORIGINAL_SHARED_SERVICES_RESTORED=YES",
                      file=sys.stderr, flush=True)

    if not activation_success:
        raise DevInstallError("DEV_INSTALL_ABORTED_OLD_SERVICES_RESTORED")

    # No client is allowed to send password without a separately audited
    # HTTPS boundary. The current installed service is strictly loopback.
    print("ORUN_S6O_REAL_ACCOUNT_BACKEND_ACTIVE=YES")
    print("ORUN_S6O_ENDPOINT=http://127.0.0.1:18781")
    print("ORUN_S6O_OWNER_USERNAME=" + USERNAME)
    print("ORUN_S6O_PC_MIRROR_ED25519_COMPATIBLE=YES")
    print("ORUN_S6O_SHARED_TIMER=ACTIVE")
    print("ORUN_S6O_OBSERVATION_BACKEND_PC_EDGE=UNTOUCHED")
    print("ORUN_S6O_PHONE_ENROLLMENT=LOOPBACK_ONLY")
    print("ORUN_S6O_REMOTE_PHONE_TLS=NOT_CONFIGURED")
    print("ORUN_S6O_APK_BUILD=NOT_ATTEMPTED")


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Install REAL ORUN owner-account shared backend on dev PC")
    parser.add_argument("--activate-dev", action="store_true",
                        help="Explicitly update the installed shared service")
    args = parser.parse_args()
    if not args.activate_dev:
        print("ORUN_S6O_REFUSED=EXPLICIT_DEV_ACTIVATION_REQUIRED", file=sys.stderr)
        return 2
    home = Path.home()
    try:
        activate(home / "ORUN-SHARED-RUNTIME",
                 home / "ORUN-ACCOUNT-CANDIDATE-S6I",
                 home / ".config" / "systemd" / "user")
        return 0
    except (DevInstallError, ShadowRefused, OSError, sqlite3.Error,
            UnicodeError, KeyboardInterrupt, EOFError) as err:
        reason = str(err) if isinstance(err, (DevInstallError, ShadowRefused)) \
                 else "INSTALL_ERROR"
        print("ORUN_S6O_INSTALL_REFUSED=" + reason, file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
