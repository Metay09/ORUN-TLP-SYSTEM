#!/usr/bin/env python3
"""ORUN S6K — backward read compatibility of an S6J shadow with pinned S4E.

This runs the *original pinned S4E server* against a disposable THIRD copy
of S6J's migrated SQLite files, using a synthetic read-only canary bearer.
No service control, live credential read, original SQLite write, Android
change, or production rollback is performed.

A passing result is only schema/runtime compatibility, NOT a verified
consistent production rollback.
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
import shutil
import sqlite3
import stat
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[2]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from tools.ci.shadow_account_sqlite_migration_s6j import (
    DB_NAMES, INSTALLATION, PINNED_S4E, SITE, ShadowRefused,
    backup_sqlite, check_db, db_connect_ro, digest, private_path,
    verified_candidate, verified_live,
)

LEGACY_SOURCE = (
    "backend/shared_sync_store.py",
    "backend/shared_sync_http.py",
    "backend/edge_enrollment.py",
    "tools/orun_shared_pc_mirror.py",
    "tools/orun_shared_sync_loopback.py",
    "tools/orun_shared_local_pc_enroll.py",
)


def validate(live: Path, candidate: Path, root: Path) -> tuple[Path, Path]:
    verified_candidate(candidate, root)
    verified_live(live)
    old_manifest_path = live / "STAGING_MANIFEST.json"
    old_manifest = json.loads(old_manifest_path.read_text(encoding="utf-8"))
    hashes = old_manifest.get("source_sha256")
    if (old_manifest.get("source_commit") != PINNED_S4E or
            not isinstance(hashes, dict) or
            set(hashes) != set(LEGACY_SOURCE)):
        raise ShadowRefused("PINNED_S4E_SOURCE_MANIFEST_MISMATCH")
    old_runtime = live / "runtime"
    for d in (old_runtime, old_runtime / "backend", old_runtime / "tools"):
        private_path(d, directory=True)
    for relative in LEGACY_SOURCE:
        file = old_runtime / relative
        private_path(file, directory=False)
        if digest(file) != hashes[relative]:
            raise ShadowRefused("PINNED_S4E_SOURCE_HASH_MISMATCH")

    shadow = candidate / "SHADOW_S6J"
    for p in (shadow, shadow / "baseline", shadow / "migration"):
        private_path(p, directory=True)
    file = shadow / "SHADOW_MANIFEST.json"
    private_path(file, directory=False)
    try:
        m = json.loads(file.read_text(encoding="utf-8"))
        if (m["state"] != "SHADOW_ONLY_NOT_DEPLOYABLE" or
                m["site"] != SITE or m["installation"] != INSTALLATION or
                m["source_commit"] != json.loads(
                    (candidate / "SOURCE_MANIFEST.json").read_text())["source_commit"] or
                m["live_pair_atomicity"] != "NOT_PROVEN" or
                m["owner_account_store"] != "NOT_CREATED" or
                set(m["baseline_db_sha256"]) != set(DB_NAMES) or
                set(m["shadow_db_sha256"]) != set(DB_NAMES)):
            raise ShadowRefused("SHADOW_MANIFEST_MISMATCH")
    except (KeyError, TypeError, ValueError):
        raise ShadowRefused("SHADOW_MANIFEST_MISMATCH") from None
    for name in DB_NAMES:
        for segment, key in (("baseline", "baseline_db_sha256"),
                             ("migration", "shadow_db_sha256")):
            path = shadow / segment / name
            private_path(path, directory=False)
            if digest(path) != m[key][name]:
                raise ShadowRefused("SHADOW_DATABASE_HASH_MISMATCH")
            check_db(path)
    return old_runtime, shadow / "migration"


def _canary(state: Path) -> str:
    token = secrets.token_urlsafe(48)
    digest_bytes = hashlib.sha256(token.encode("ascii")).digest()
    with sqlite3.connect(state / "enrolled_edges.sqlite", timeout=5) as db:
        db.execute(
            "INSERT INTO edge_installs"
            "(installation_id,public_key,site_id,actor_id,scopes_json,"
            "token_hash,expires_at,revoked_at,key_algorithm)"
            " VALUES (?,?,?,?,?,?,?,NULL,'Ed25519')",
            ("pc-s6k-canary", os.urandom(32), SITE, "owner",
             json.dumps(["zone:read", "group:read", "zone_target:read"]),
             digest_bytes, int(time.time()) + 600),
        )
    return token


def _get(port: int, endpoint: str, token: str) -> tuple[int, dict]:
    conn = http.client.HTTPConnection("127.0.0.1", port, timeout=4)
    try:
        conn.request("GET", endpoint, headers={
            "Authorization": "Bearer " + token,
            "Accept": "application/json",
        })
        reply = conn.getresponse()
        return reply.status, json.loads(reply.read())
    finally:
        conn.close()


def _stop(process: subprocess.Popen) -> None:
    if process.poll() is None:
        process.terminate()
    try:
        process.communicate(timeout=5)
    except subprocess.TimeoutExpired:
        process.kill()
        process.communicate(timeout=5)


def probe(live: Path, candidate: Path, *, root: Path = ROOT) -> None:
    pinned, migrated = validate(live, candidate, root)
    # Keep all private data inside the already owner-private candidate folder.
    # tempfile always creates a new 0700 directory and removes it afterward.
    with tempfile.TemporaryDirectory(prefix="S6K_ROLLBACK_PROBE_", dir=candidate) as tmp:
        work = Path(tmp) / "backend"
        work.mkdir(mode=0o700)
        for name in DB_NAMES:
            backup_sqlite(migrated / name, work / name)
        # A real original PC bearer is neither available nor needed; canary
        # affects only temporary copy and has read-only shared-data scopes.
        canary = _canary(work)
        script = pinned / "tools" / "orun_shared_sync_loopback.py"
        env = dict(os.environ)
        env.pop("PYTHONPATH", None)
        process = subprocess.Popen(
            [sys.executable, "-I", "-u", str(script),
             "--state-dir", str(work), "--bind", "127.0.0.1",
             "--port", "0", "--reuse-state"],
            cwd=pinned, env=env, stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            text=True, bufsize=1,
        )
        try:
            if process.stdout is None:
                raise ShadowRefused("PINNED_RUNTIME_OUTPUT_MISSING")
            ready, _, _ = select.select([process.stdout], [], [], 10)
            if not ready:
                raise ShadowRefused("PINNED_RUNTIME_START_TIMEOUT")
            line = process.stdout.readline().strip()
            prefix = "ORUN_SHARED_BIND=127.0.0.1:"
            if not line.startswith(prefix):
                raise ShadowRefused("PINNED_RUNTIME_START_FAILED")
            port = int(line[len(prefix):])
            if not 1 <= port <= 65535:
                raise ShadowRefused("PINNED_RUNTIME_PORT_INVALID")
            status, identity = _get(port, "/api/v1/sync/identity", canary)
            if status != 200 or identity != {
                    "site": SITE, "installation": "pc-s6k-canary"}:
                raise ShadowRefused("PINNED_RUNTIME_IDENTITY_FAILED")
            status, response = _get(port, "/api/v1/sync?cursor=0", canary)
            if (status != 200 or not isinstance(response.get("changes"), list)
                    or not isinstance(response.get("cursor"), int)
                    or not isinstance(response.get("has_more"), bool)):
                raise ShadowRefused("PINNED_RUNTIME_SHARED_READ_FAILED")
            if _get(port, "/api/v1/account/me", canary)[0] != 404:
                raise ShadowRefused("PINNED_RUNTIME_ACCOUNT_GATE_NOT_CLOSED")
        except (OSError, ValueError, json.JSONDecodeError, http.client.HTTPException):
            raise ShadowRefused("PINNED_RUNTIME_HTTP_FAILED") from None
        finally:
            _stop(process)

    # Never present a schema compatibility pass as a successful physical
    # cutover; the live service and its SQLite files were not touched.
    print("ORUN_S6K_PINNED_S4E_BACKWARD_READ_PASS")
    print("LEGACY_PC_READ_ONLY_CANARY=PASS")
    print("REAL_PC_TOKEN_ACCESSED=NO")
    print("LIVE_DATABASE_WRITE=NOT_ATTEMPTED")
    print("LIVE_SERVICE_RESTART=NOT_ATTEMPTED")
    print("CONSISTENT_CUTOVER_BACKUP=NOT_PROVEN")
    print("PRODUCTION_ROLLBACK_APPROVAL=NO")


def main() -> int:
    parser = argparse.ArgumentParser(description="ORUN S6K isolated legacy rollback probe")
    parser.add_argument("--live-root", required=True)
    parser.add_argument("--candidate", required=True)
    a = parser.parse_args()
    try:
        probe(Path(a.live_root), Path(a.candidate))
    except (ShadowRefused, OSError, sqlite3.Error,
            subprocess.TimeoutExpired) as error:
        code = str(error) if isinstance(error, ShadowRefused) else "PROBE_FAILED"
        print("ORUN_S6K_PROBE_REFUSED=" + code, file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
