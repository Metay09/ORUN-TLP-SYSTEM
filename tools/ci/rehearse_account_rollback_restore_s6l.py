#!/usr/bin/env python3
"""ORUN S6L: disposable checkpoint/restore mechanics with real S6J shadow data.

NEVER copies from live running databases, stops services or touches the
existing S6J shadow. Uses only hash-verified S6J baseline/migration snapshots
and a private temporary workspace under the existing S6I candidate.

A pass proves restore MECHANICS, not an atomic cross-database cutover backup.
"""
from __future__ import annotations

import argparse
import hashlib
import http.client
import json
import os
from pathlib import Path
import secrets
import select
import sqlite3
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[2]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from tools.ci.probe_s4e_shadow_rollback_s6k import (
    _get, _stop, validate,
)
from tools.ci.shadow_account_sqlite_migration_s6j import (
    DB_NAMES, INSTALLATION, SITE, ShadowRefused, backup_sqlite,
    check_db, db_connect_ro, migrate_only_shadow,
)

RUNTIME_OP = "s6l-trial-operation"
RUNTIME_GROUP = "s6l-trial-group"


def _logical_digest(path: Path) -> str:
    # Byte hashes of independently backed-up SQLite files can differ despite
    # identical logical content (e.g. checkpoint and journal layout).
    with db_connect_ro(path) as connection:
        stream = hashlib.sha256()
        for line in connection.iterdump():
            stream.update(line.encode("utf-8"))
            stream.update(b"\n")
        return stream.hexdigest()


def _s6l_candidate_write(candidate: Path, state: Path) -> None:
    """Open a synthetic future account store and append a group to a CLONE."""
    root = candidate / "runtime"
    code = (
        "import sys; "
        f"sys.path.insert(0,{str(root)!r}); "
        "from backend.account_login import AccountStore; "
        "from backend.shared_sync_store import SharedStore,Actor; "
        f"AccountStore({str(state / 'owner_accounts.sqlite')!r}); "
        f"store=SharedStore({str(state / 'shared_resources.sqlite')!r}); "
        "who=Actor('orun-main','owner','s6l-synthetic',frozenset({'group:write'})); "
        "result=store.mutate(who,[{'op_id':'s6l-trial-operation',"
        "'kind':'group','id':'s6l-trial-group','action':'UPSERT',"
        "'base_revision':0,'body':{'name':'Temporary rollback drill',"
        "'device_ids':[]}}]); "
        "assert result[0]['status']=='ACCEPTED'; "
        "print('S6L_SYNTHETIC_ACCOUNT_WRITE_PASS')"
    )
    env = dict(os.environ)
    env.pop("PYTHONPATH", None)
    env["PYTHONDONTWRITEBYTECODE"] = "1"
    result = subprocess.run(
        [sys.executable, "-I", "-B", "-c", code],
        cwd=root, env=env, capture_output=True, text=True,
        timeout=20, check=False,
    )
    if (result.returncode != 0 or
            "S6L_SYNTHETIC_ACCOUNT_WRITE_PASS" not in result.stdout):
        raise ShadowRefused("CANDIDATE_SYNTHETIC_WRITE_FAILED")


def _verify_reverted_with_s4e(pinned: Path, restored: Path) -> None:
    # A random read-only token appears only in restored disposable SQLite.
    token = secrets.token_urlsafe(48)
    with sqlite3.connect(restored / "enrolled_edges.sqlite", timeout=5) as db:
        # Restore is from S4E baseline, so its old table has NO
        # key_algorithm column. Do not use S6K's newer-schema canary.
        cols = {row[1] for row in db.execute(
            "PRAGMA table_info(edge_installs)")}
        if "key_algorithm" in cols:
            raise ShadowRefused("RESTORED_SCHEMA_IS_NOT_LEGACY")
        db.execute(
            "INSERT INTO edge_installs "
            "(installation_id,public_key,site_id,actor_id,scopes_json,"
            "token_hash,expires_at,revoked_at) VALUES(?,?,?,?,?,?,?,NULL)",
            ("pc-s6l-canary", os.urandom(32), SITE, "owner",
             json.dumps(["zone:read", "group:read", "zone_target:read"]),
             hashlib.sha256(token.encode("ascii")).digest(),
             int(time.time()) + 600),
        )
    script = pinned / "tools" / "orun_shared_sync_loopback.py"
    env = dict(os.environ)
    env.pop("PYTHONPATH", None)
    env["PYTHONDONTWRITEBYTECODE"] = "1"
    process = subprocess.Popen(
        [sys.executable, "-I", "-B", "-u", str(script),
         "--state-dir", str(restored), "--bind", "127.0.0.1",
         "--port", "0", "--reuse-state"],
        cwd=pinned, env=env,
        stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
        stderr=subprocess.PIPE, text=True, bufsize=1,
    )
    try:
        if process.stdout is None:
            raise ShadowRefused("LEGACY_RESTORE_STDOUT_UNAVAILABLE")
        readable, _, _ = select.select([process.stdout], [], [], 10)
        if not readable:
            raise ShadowRefused("LEGACY_RESTORE_SERVER_TIMEOUT")
        first = process.stdout.readline().strip()
        prefix = "ORUN_SHARED_BIND=127.0.0.1:"
        if not first.startswith(prefix):
            raise ShadowRefused("LEGACY_RESTORE_SERVER_FAILED")
        port = int(first[len(prefix):])
        if not 1 <= port <= 65535:
            raise ShadowRefused("LEGACY_RESTORE_PORT_INVALID")
        status, identity = _get(port, "/api/v1/sync/identity", token)
        if status != 200 or identity != {
                "site": SITE, "installation": "pc-s6l-canary"}:
            raise ShadowRefused("LEGACY_RESTORE_IDENTITY_MISMATCH")
        status, result = _get(port, "/api/v1/sync?cursor=0", token)
        if status != 200 or not isinstance(result.get("changes"), list):
            raise ShadowRefused("LEGACY_RESTORE_SHARED_READ_FAILED")
        if any(item.get("id") == RUNTIME_GROUP for item in result["changes"]):
            raise ShadowRefused("ROLLED_BACK_TEST_WRITE_STILL_PRESENT")
        if _get(port, "/api/v1/account/me", token)[0] != 404:
            raise ShadowRefused("ROLLED_BACK_ACCOUNT_ENDPOINT_STILL_PRESENT")
    except (OSError, ValueError, json.JSONDecodeError,
            http.client.HTTPException):
        raise ShadowRefused("LEGACY_RESTORE_HTTP_FAILED") from None
    finally:
        _stop(process)


def rehearse(live: Path, candidate: Path, *, root: Path = ROOT) -> None:
    pinned, _ = validate(live, candidate, root)
    baseline = candidate / "SHADOW_S6J" / "baseline"
    with tempfile.TemporaryDirectory(prefix="S6L_COLD_RESTORE_TEST_",
                                     dir=candidate) as directory:
        stage = Path(directory)
        checkpoint = stage / "checkpoint"
        working = stage / "candidate"
        restored = stage / "restored"
        for folder in (checkpoint, working, restored):
            folder.mkdir(mode=0o700)
        for name in DB_NAMES:
            backup_sqlite(baseline / name, checkpoint / name)
        sealed = {name: _logical_digest(checkpoint / name)
                  for name in DB_NAMES}
        for name in DB_NAMES:
            backup_sqlite(checkpoint / name, working / name)
        # This changes only the WORKING copy, not checkpoint/shadow/live.
        migrate_only_shadow(candidate, working)
        _s6l_candidate_write(candidate, working)
        if not (working / "owner_accounts.sqlite").is_file():
            raise ShadowRefused("SYNTHETIC_OWNER_ACCOUNT_STORE_NOT_CREATED")
        if _logical_digest(working / "shared_resources.sqlite") == sealed[
                "shared_resources.sqlite"]:
            raise ShadowRefused("CANDIDATE_WRITE_NOT_OBSERVED")

        # Restore to a third, new empty directory. No overwrite of existing
        # state, and no time-of-check/time-of-use window on live databases.
        for name in DB_NAMES:
            backup_sqlite(checkpoint / name, restored / name)
            check_db(restored / name)
            if _logical_digest(restored / name) != sealed[name]:
                raise ShadowRefused("RESTORE_LOGICAL_CONTENT_MISMATCH")
            if _logical_digest(checkpoint / name) != sealed[name]:
                raise ShadowRefused("CHECKPOINT_CONTENT_CHANGED")
        if (restored / "owner_accounts.sqlite").exists():
            raise ShadowRefused("RESTORE_LEAKED_NEW_ACCOUNT_STATE")
        _verify_reverted_with_s4e(pinned, restored)
        if any(_logical_digest(checkpoint / name) != sealed[name]
               for name in DB_NAMES):
            raise ShadowRefused("RESTORE_CHECKPOINT_MUTATED")

    print("ORUN_S6L_DISPOSABLE_ROLLBACK_RESTORE_PASS")
    print("S6J_BASELINE_AND_MIGRATION_CHANGED=NO")
    print("REAL_PC_TOKEN_USED=NO")
    print("COLD_ATOMIC_PRODUCTION_BACKUP=NOT_YET_CREATED")
    print("LIVE_SERVICE_RESTART=NOT_ATTEMPTED")
    print("PRODUCTION_ROLLBACK_APPROVAL=NO")


def main() -> int:
    parser = argparse.ArgumentParser(
        description="ORUN S6L disposable rollback rehearsal; NEVER live restore")
    parser.add_argument("--live-root", required=True)
    parser.add_argument("--candidate", required=True)
    args = parser.parse_args()
    try:
        rehearse(Path(args.live_root), Path(args.candidate))
        return 0
    except (ShadowRefused, OSError, sqlite3.Error,
            subprocess.TimeoutExpired) as error:
        code = str(error) if isinstance(error, ShadowRefused) else "RESTORE_PROBE_FAILED"
        print("ORUN_S6L_RESTORE_REFUSED=" + code, file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
