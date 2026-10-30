#!/usr/bin/env python3
"""ORUN S6J — read-only SQLite shadow copy and detached schema rehearsal.

Copies the two existing S4E Backend SQLite files with the SQLite Online Backup
API, then migrates ONLY a second private copy with the pinned S6I Python code.
Does not read the PC bearer file, print any credential, change systemd, start
the live server, create an owner account, or write the original databases.

The pair of backups is SEQUENTIAL, not an atomic two-database snapshot.
It is a diagnostic rehearsal, NOT cutover data nor an approved rollback.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import sqlite3
import stat
import subprocess
import sys
from urllib.parse import quote

from tools.ci.stage_account_runtime_source_s6i import SOURCES

PINNED_S4E = "264b4117a4d9fdda2b3ed8f328d0e02f17ee52bc"
SITE = "orun-main"
INSTALLATION = "pc-main"
PORT = 18781
DB_NAMES = ("shared_resources.sqlite", "enrolled_edges.sqlite")
LEGACY_COLUMNS = {
    "edge_installs": ("installation_id", "public_key", "site_id",
                      "actor_id", "scopes_json", "token_hash",
                      "expires_at", "revoked_at"),
    "edge_pending": ("request_id", "installation_id", "public_key",
                     "challenge", "expires_at", "owner_code_hash",
                     "site_id", "actor_id", "scopes_json"),
}


class ShadowRefused(ValueError):
    pass


def private_path(path: Path, *, directory: bool) -> None:
    try:
        item = path.lstat()
    except OSError:
        raise ShadowRefused("PRIVATE_PATH_MISSING") from None
    valid_type = stat.S_ISDIR(item.st_mode) if directory else stat.S_ISREG(item.st_mode)
    if (not valid_type or item.st_uid != os.geteuid() or
            stat.S_IMODE(item.st_mode) & 0o077):
        raise ShadowRefused("PRIVATE_PATH_UNSAFE")


def digest(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            h.update(block)
    return h.hexdigest()


def verified_candidate(candidate: Path, source_root: Path) -> dict:
    for path in (candidate, candidate / "runtime",
                 candidate / "runtime" / "backend",
                 candidate / "runtime" / "tools"):
        private_path(path, directory=True)
    manifest_file = candidate / "SOURCE_MANIFEST.json"
    private_path(manifest_file, directory=False)
    try:
        manifest = json.loads(manifest_file.read_text(encoding="utf-8"))
        if (manifest["state"] != "SOURCE_ONLY_NOT_INSTALLED" or
                manifest["account_mode"] != "NOT_ENABLED" or
                set(manifest["sha256"]) != set(SOURCES)):
            raise ShadowRefused("SOURCE_MANIFEST_NOT_VALID")
        for rel in SOURCES:
            output = candidate / "runtime" / rel
            private_path(output, directory=False)
            original = source_root / rel
            if not original.is_file() or original.is_symlink():
                raise ShadowRefused("SOURCE_NOT_AVAILABLE")
            expected = manifest["sha256"][rel]
            if digest(output) != expected or digest(original) != expected:
                raise ShadowRefused("SOURCE_HASH_MISMATCH")
    except (KeyError, TypeError, json.JSONDecodeError):
        raise ShadowRefused("SOURCE_MANIFEST_NOT_VALID") from None
    return manifest


def verified_live(live: Path) -> Path:
    for p in (live, live / "state", live / "state" / "backend"):
        private_path(p, directory=True)
    manifest_file = live / "STAGING_MANIFEST.json"
    private_path(manifest_file, directory=False)
    try:
        manifest = json.loads(manifest_file.read_text(encoding="utf-8"))
        if (manifest["site"] != SITE or
                manifest["installation"] != INSTALLATION or
                manifest["loopback_port"] != PORT or
                manifest["source_commit"] != PINNED_S4E):
            raise ShadowRefused("LIVE_INSTALLATION_MISMATCH")
    except (KeyError, TypeError, json.JSONDecodeError):
        raise ShadowRefused("LIVE_INSTALLATION_MISMATCH") from None
    directory = live / "state" / "backend"
    for name in DB_NAMES:
        private_path(directory / name, directory=False)
    return directory


def db_connect_ro(path: Path) -> sqlite3.Connection:
    # mode=ro uses the existing WAL while refusing SQL writes.
    connection = sqlite3.connect(
        "file:" + quote(str(path), safe="/") + "?mode=ro",
        uri=True, timeout=5.0)
    connection.execute("PRAGMA query_only=ON")
    return connection


def check_db(path: Path) -> None:
    with db_connect_ro(path) as db:
        answer = db.execute("PRAGMA quick_check").fetchone()
        if not answer or answer[0] != "ok":
            raise ShadowRefused("SQLITE_INTEGRITY_FAILED")


def backup_sqlite(src: Path, dst: Path) -> None:
    if dst.exists() or dst.is_symlink():
        raise ShadowRefused("BACKUP_DESTINATION_EXISTS")
    with db_connect_ro(src) as origin:
        with sqlite3.connect(str(dst), timeout=5.0) as target:
            origin.backup(target, pages=64, sleep=0.05)
    dst.chmod(0o600)
    private_path(dst, directory=False)
    check_db(dst)


def rows(path: Path, table: str) -> list[tuple]:
    columns = LEGACY_COLUMNS[table]
    with db_connect_ro(path) as db:
        return db.execute(
            "SELECT " + ",".join(columns) + " FROM " + table +
            " ORDER BY " + columns[0]).fetchall()


def migrate_only_shadow(candidate: Path, rehearsal: Path) -> None:
    # Isolated Python process, loading modules solely from the candidate.
    # Does NOT import code from the developer checkout into migrated state.
    root = candidate / "runtime"
    program = (
        "import sys; "
        f"sys.path.insert(0,{str(root)!r}); "
        "from backend.edge_enrollment import EdgeEnrollment; "
        f"EdgeEnrollment({str(rehearsal / 'enrolled_edges.sqlite')!r}); "
        "print('SHADOW_ONLY_SCHEMA_UPGRADE_PASS')"
    )
    env = dict(os.environ)
    env.pop("PYTHONPATH", None)
    result = subprocess.run(
        [sys.executable, "-I", "-B", "-c", program],
        cwd=root, env=env, capture_output=True, text=True, timeout=20)
    if (result.returncode != 0 or
            "SHADOW_ONLY_SCHEMA_UPGRADE_PASS" not in result.stdout):
        # Never display stderr, paths or private database content.
        raise ShadowRefused("SHADOW_SCHEMA_UPGRADE_FAILED")


def rehearse(live: Path, candidate: Path,
             *, source_root: Path) -> dict:
    manifest = verified_candidate(candidate, source_root)
    original = verified_live(live)
    # Never allow a caller to point the source at the candidate or its shadow.
    if candidate.resolve() == live.resolve() or (
            candidate.resolve().is_relative_to(live.resolve()) or
            live.resolve().is_relative_to(candidate.resolve())):
        raise ShadowRefused("SOURCE_DESTINATION_OVERLAP")
    stage = candidate / "SHADOW_S6J"
    if stage.exists() or stage.is_symlink():
        raise ShadowRefused("SHADOW_ALREADY_EXISTS_NO_OVERWRITE")

    previous = os.umask(0o077)
    created = False
    try:
        stage.mkdir(mode=0o700)
        created = True
        baseline = stage / "baseline"
        migration = stage / "migration"
        baseline.mkdir(mode=0o700)
        migration.mkdir(mode=0o700)
        for name in DB_NAMES:
            check_db(original / name)
            backup_sqlite(original / name, baseline / name)
            backup_sqlite(baseline / name, migration / name)

        original_pc = rows(baseline / "enrolled_edges.sqlite", "edge_installs")
        pending = rows(baseline / "enrolled_edges.sqlite", "edge_pending")
        matches = [x for x in original_pc if x[0] == INSTALLATION]
        if (len(matches) != 1 or matches[0][2] != SITE or
                matches[0][6] <= __import__("time").time() or
                matches[0][7] is not None):
            raise ShadowRefused("EXPECTED_PC_AUTHORIZATION_NOT_ACTIVE")
        shared_hash = digest(migration / "shared_resources.sqlite")

        migrate_only_shadow(candidate, migration)

        if (rows(migration / "enrolled_edges.sqlite", "edge_installs")
                != original_pc or
                rows(migration / "enrolled_edges.sqlite", "edge_pending")
                != pending):
            raise ShadowRefused("LEGACY_REGISTRY_CHANGED")
        if digest(migration / "shared_resources.sqlite") != shared_hash:
            raise ShadowRefused("SHARED_LEDGER_CHANGED")
        check_db(migration / "enrolled_edges.sqlite")
        with db_connect_ro(migration / "enrolled_edges.sqlite") as db:
            for table, column in (("edge_installs", "key_algorithm"),
                                  ("edge_pending", "purpose")):
                names = {r[1] for r in db.execute(
                    f"PRAGMA table_info({table})")}
                if column not in names:
                    raise ShadowRefused("MIGRATION_COLUMN_NOT_FOUND")
            key_algorithm = db.execute(
                "SELECT key_algorithm FROM edge_installs "
                "WHERE installation_id=?", (INSTALLATION,)).fetchone()
            if not key_algorithm or key_algorithm[0] != "Ed25519":
                raise ShadowRefused("LEGACY_PC_KEY_ALGORITHM_LOST")

        result = {
            "state": "SHADOW_ONLY_NOT_DEPLOYABLE",
            "site": SITE,
            "installation": INSTALLATION,
            "source_commit": manifest["source_commit"],
            "source_sha256": manifest["sha256"],
            "baseline_db_sha256": {
                name: digest(baseline / name) for name in DB_NAMES},
            "shadow_db_sha256": {
                name: digest(migration / name) for name in DB_NAMES},
            "owner_account_store": "NOT_CREATED",
            "live_pair_atomicity": "NOT_PROVEN",
            "systemd_activation": "NOT_ATTEMPTED",
            "rollback_approval": "NO",
        }
        output = stage / "SHADOW_MANIFEST.json"
        with output.open("x", encoding="utf-8") as stream:
            json.dump(result, stream, indent=2, sort_keys=True)
            stream.write("\n")
        output.chmod(0o600)
        return result
    except BaseException:
        # Only our newly created directory. Preserve ALL preexisting content.
        if created:
            shutil.rmtree(stage)
        raise
    finally:
        os.umask(previous)


def main() -> int:
    parser = argparse.ArgumentParser(description="ORUN S6J private shadow rehearsal")
    parser.add_argument("--live-root", required=True)
    parser.add_argument("--candidate", required=True)
    args = parser.parse_args()
    try:
        rehearse(Path(args.live_root), Path(args.candidate),
                 source_root=Path(__file__).resolve().parents[2])
    except (ShadowRefused, OSError, sqlite3.Error, ValueError,
            subprocess.TimeoutExpired) as error:
        reason = str(error) if isinstance(error, ShadowRefused) else "REHEARSAL_ERROR"
        print("ORUN_S6J_SHADOW_REFUSED=" + reason, file=sys.stderr)
        return 2
    print("ORUN_S6J_SHADOW_SCHEMA_REHEARSAL_PASS")
    print("LEGACY_PC_ROWS_AND_TOKEN_HASHES_UNCHANGED_IN_SHADOW=YES")
    print("SHARED_LEDGER_UNCHANGED_IN_SHADOW=YES")
    print("SHADOW_IS_CUTOVER_BACKUP=NO")
    print("SOURCE_DATABASES_MODIFICATION=NOT_ATTEMPTED")
    print("SYSTEMD_RESTART=NOT_ATTEMPTED")
    print("PRODUCTION_ACTIVATION=NO")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
