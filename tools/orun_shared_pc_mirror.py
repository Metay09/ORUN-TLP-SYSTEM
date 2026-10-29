#!/usr/bin/env python3
"""ORUN S4A: independent, read-only PC Edge shared-resource mirror.

Opt-in CLI only. Does not run as a service, modify the existing PC Edge
ledger, upload records, distribute geofences, issue Tracker commands, or
silently reuse the shared bench bearer. Requires Step3C per-install token.

Network: authenticated GET /api/v1/sync over verified HTTPS; HTTP only on
127.0.0.1 for isolated tests. Every page and its cursor are one SQLite
transaction. Original on-phone or on-PC resource drafts are never written.
"""
from __future__ import annotations

import argparse
from contextlib import contextmanager
import fcntl
import json
import os
from pathlib import Path
import re
import sqlite3
import stat
import sys
from typing import Any, Iterator
from urllib import error, parse, request

from backend.shared_sync_store import KINDS, SyncInvalid, _payload

ID = re.compile(r"^[A-Za-z0-9._-]{1,128}$")
TOKEN = re.compile(r"^[A-Za-z0-9_-]{48,128}$")
LIMIT = 100
MAX_PAGES = 10
MAX_JSON_BYTES = 512 * 1024


class MirrorError(RuntimeError):
    """Safe-to-display failure. Never include bearer, URL query or payload."""


def validate_origin(raw: str) -> str:
    try:
        value = parse.urlsplit(raw)
        host = value.hostname
        port = value.port  # Raises ValueError for malformed ports.
    except (ValueError, TypeError) as exc:
        raise MirrorError("SYNC_ORIGIN_INVALID") from exc
    if (value.username is not None or value.password is not None or
            value.query or value.fragment or value.path not in ("", "/") or
            not host or not value.netloc):
        raise MirrorError("SYNC_ORIGIN_INVALID")
    if value.scheme == "https":
        pass
    elif value.scheme == "http" and host == "127.0.0.1":
        pass
    else:
        raise MirrorError("SYNC_HTTPS_REQUIRED")
    if host in ("localhost", "127.0.0.1") and value.scheme == "http" and host != "127.0.0.1":
        raise MirrorError("SYNC_HTTPS_REQUIRED")
    if port is not None and not 1 <= port <= 65535:
        raise MirrorError("SYNC_ORIGIN_INVALID")
    return f"{value.scheme}://{value.netloc}"


def _private_file(path: Path) -> None:
    if not path.parent.is_dir() or path.is_symlink():
        raise MirrorError("SYNC_DB_PATH_INVALID")
    if path.exists():
        mode = path.stat().st_mode
        if not stat.S_ISREG(mode) or stat.S_IMODE(mode) & 0o077:
            raise MirrorError("SYNC_DB_PERMISSIONS")
        return
    flags = os.O_CREAT | os.O_EXCL | os.O_WRONLY | getattr(os, "O_NOFOLLOW", 0)
    try:
        fd = os.open(path, flags, 0o600)
    except FileExistsError:
        # A concurrent creator must still satisfy the same safe mode checks.
        if path.is_symlink() or not path.is_file() or (
                stat.S_IMODE(path.stat().st_mode) & 0o077):
            raise MirrorError("SYNC_DB_PERMISSIONS")
    else:
        os.close(fd)


def _validated_body(kind: str, body: Any, deleted: bool) -> str | None:
    if deleted:
        if body is not None:
            raise MirrorError("SYNC_TOMBSTONE_INVALID")
        return None
    try:
        canonical = _payload(kind, body)
    except (SyncInvalid, ValueError, TypeError) as exc:
        raise MirrorError("SYNC_RESOURCE_INVALID") from exc
    if canonical != body:
        raise MirrorError("SYNC_RESOURCE_NONCANONICAL")
    return json.dumps(body, sort_keys=True, ensure_ascii=False, separators=(",", ":"),
                      allow_nan=False)


class PcSharedMirror:
    def __init__(self, path: str | Path, site: str, installation: str,
                 origin: str):
        if not isinstance(site, str) or not ID.fullmatch(site) or (
                not isinstance(installation, str) or not ID.fullmatch(installation)):
            raise MirrorError("SYNC_IDENTITY_INVALID")
        self.path = Path(path)
        self.site = site
        self.installation = installation
        self.origin = validate_origin(origin)
        _private_file(self.path)
        # The sidecar is lock-only and never stores credentials.
        self.lock_path = self.path.with_name(self.path.name + ".lock")
        _private_file(self.lock_path)
        with self._session() as db:
            db.executescript("""
                CREATE TABLE IF NOT EXISTS mirror_meta(
                    key TEXT PRIMARY KEY, value TEXT NOT NULL);
                CREATE TABLE IF NOT EXISTS mirror_resources(
                    kind TEXT NOT NULL, rid TEXT NOT NULL,
                    revision INTEGER NOT NULL, deleted INTEGER NOT NULL,
                    body_json TEXT, sequence INTEGER NOT NULL,
                    PRIMARY KEY(kind,rid));
            """)
            required = {"site": self.site, "installation": self.installation,
                        "origin": self.origin, "cursor": "0"}
            existing = dict(db.execute("SELECT key,value FROM mirror_meta"))
            if existing:
                if (set(existing) != set(required) or
                        any(existing[k] != v for k, v in required.items() if k != "cursor") or
                        not existing["cursor"].isascii() or
                        not existing["cursor"].isdecimal() or
                        int(existing["cursor"]) > 2**63 - 1):
                    raise MirrorError("SYNC_MIRROR_BOUND_TO_ANOTHER_ACCOUNT")
            else:
                db.executemany("INSERT INTO mirror_meta VALUES(?,?)",
                               list(required.items()))
            self._check_rows(db)

    @contextmanager
    def _session(self) -> Iterator[sqlite3.Connection]:
        db = sqlite3.connect(str(self.path), timeout=8)
        try:
            db.execute("PRAGMA synchronous=FULL")
            db.execute("PRAGMA busy_timeout=8000")
            db.row_factory = sqlite3.Row
            yield db
            db.commit()
        except BaseException:
            db.rollback()
            raise
        finally:
            db.close()

    @contextmanager
    def _lock(self) -> Iterator[None]:
        fd = os.open(self.lock_path, os.O_RDONLY | getattr(os, "O_NOFOLLOW", 0))
        try:
            fcntl.flock(fd, fcntl.LOCK_EX)
            yield
        finally:
            fcntl.flock(fd, fcntl.LOCK_UN)
            os.close(fd)

    @staticmethod
    def _check_rows(db: sqlite3.Connection) -> None:
        if db.execute("PRAGMA quick_check").fetchone()[0] != "ok":
            raise MirrorError("SYNC_MIRROR_CORRUPT")
        for row in db.execute("SELECT kind,rid,revision,deleted,body_json,sequence "
                              "FROM mirror_resources"):
            if (row["kind"] not in KINDS or not ID.fullmatch(row["rid"]) or
                    type(row["revision"]) is not int or row["revision"] < 1 or
                    row["deleted"] not in (0, 1) or
                    type(row["sequence"]) is not int or row["sequence"] < 1):
                raise MirrorError("SYNC_MIRROR_CORRUPT")
            try:
                body = json.loads(row["body_json"]) if row["body_json"] is not None else None
                _validated_body(row["kind"], body, bool(row["deleted"]))
            except (ValueError, TypeError, MirrorError) as exc:
                raise MirrorError("SYNC_MIRROR_CORRUPT") from exc

    def cursor(self) -> int:
        with self._session() as db:
            return int(db.execute("SELECT value FROM mirror_meta WHERE key='cursor'").fetchone()[0])

    def apply_page(self, raw: Any, requested_cursor: int) -> tuple[int, bool, int]:
        if (not isinstance(raw, dict) or set(raw) != {"changes", "cursor", "has_more"} or
                not isinstance(raw["changes"], list) or
                len(raw["changes"]) > 200 or type(raw["cursor"]) is not int or
                not requested_cursor <= raw["cursor"] <= 2**63 - 1 or
                type(raw["has_more"]) is not bool):
            raise MirrorError("SYNC_PAGE_INVALID")
        cursor, changes, has_more = raw["cursor"], raw["changes"], raw["has_more"]
        if (not changes and (cursor != requested_cursor or has_more)):
            raise MirrorError("SYNC_CURSOR_INVALID")
        prepared = []
        last = requested_cursor
        for item in changes:
            if (not isinstance(item, dict) or
                    set(item) != {"sequence", "kind", "id", "revision", "deleted", "body"} or
                    type(item["sequence"]) is not int or
                    not last < item["sequence"] <= cursor or
                    item["kind"] not in KINDS or not isinstance(item["id"], str) or
                    not ID.fullmatch(item["id"]) or
                    type(item["revision"]) is not int or
                    not 1 <= item["revision"] <= 2**53 or
                    type(item["deleted"]) is not bool):
                raise MirrorError("SYNC_CHANGE_INVALID")
            body_json = _validated_body(item["kind"], item["body"], item["deleted"])
            prepared.append((item["kind"], item["id"], item["revision"],
                             int(item["deleted"]), body_json, item["sequence"]))
            last = item["sequence"]
        if changes and last != cursor:
            raise MirrorError("SYNC_CURSOR_INVALID")
        with self._session() as db:
            db.execute("BEGIN IMMEDIATE")
            actual = int(db.execute("SELECT value FROM mirror_meta WHERE key='cursor'").fetchone()[0])
            if actual != requested_cursor:
                raise MirrorError("SYNC_CURSOR_RACE")
            self._check_rows(db)
            for kind, rid, revision, deleted, body, seq in prepared:
                old = db.execute("SELECT revision,deleted,body_json "
                                 "FROM mirror_resources WHERE kind=? AND rid=?",
                                 (kind, rid)).fetchone()
                if old and (revision < old["revision"] or
                            (revision == old["revision"] and
                             (deleted != old["deleted"] or body != old["body_json"]))):
                    raise MirrorError("SYNC_REVISION_CONFLICT")
                db.execute("""
                    INSERT INTO mirror_resources(kind,rid,revision,deleted,body_json,sequence)
                    VALUES(?,?,?,?,?,?)
                    ON CONFLICT(kind,rid) DO UPDATE SET
                        revision=excluded.revision, deleted=excluded.deleted,
                        body_json=excluded.body_json,sequence=excluded.sequence
                """, (kind, rid, revision, deleted, body, seq))
            db.execute("UPDATE mirror_meta SET value=? WHERE key='cursor'", (str(cursor),))
        return len(prepared), has_more, cursor

    def snapshot(self) -> dict[str, Any]:
        with self._session() as db:
            self._check_rows(db)
            cursor = int(db.execute("SELECT value FROM mirror_meta WHERE key='cursor'").fetchone()[0])
            rows = db.execute("SELECT kind,rid,revision,deleted,body_json "
                              "FROM mirror_resources ORDER BY kind,rid").fetchall()
        return {"site": self.site, "installation": self.installation, "cursor": cursor,
                "resources": [{"kind": r["kind"], "id": r["rid"],
                               "revision": r["revision"], "deleted": bool(r["deleted"]),
                               "body": json.loads(r["body_json"]) if r["body_json"] else None}
                              for r in rows]}


class _NoRedirect(request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        return None


def _pull(origin: str, bearer: str, cursor: int) -> Any:
    if not isinstance(bearer, str) or not TOKEN.fullmatch(bearer):
        raise MirrorError("SYNC_ENROLLMENT_REQUIRED")
    url = f"{origin}/api/v1/sync?cursor={cursor}&limit={LIMIT}"
    req = request.Request(url, headers={
        "Authorization": "Bearer " + bearer,
        "Accept": "application/json",
        "Cache-Control": "no-store",
    }, method="GET")
    try:
        with request.build_opener(_NoRedirect()).open(req, timeout=15) as res:
            if res.status != 200:
                raise MirrorError("SYNC_HTTP_FAILED")
            if "application/json" not in res.headers.get("Content-Type", "").lower():
                raise MirrorError("SYNC_PROTOCOL_INVALID")
            length = res.headers.get("Content-Length")
            if length and (not length.isascii() or not length.isdecimal() or
                           int(length) > MAX_JSON_BYTES):
                raise MirrorError("SYNC_RESPONSE_TOO_BIG")
            data = res.read(MAX_JSON_BYTES + 1)
            if len(data) > MAX_JSON_BYTES:
                raise MirrorError("SYNC_RESPONSE_TOO_BIG")
            return json.loads(data.decode("utf-8"))
    except error.HTTPError as exc:
        if exc.code == 401:
            raise MirrorError("SYNC_AUTH_EXPIRED") from None
        if exc.code == 403:
            raise MirrorError("SYNC_FORBIDDEN") from None
        raise MirrorError("SYNC_HTTP_FAILED") from None
    except (error.URLError, TimeoutError, OSError) as exc:
        raise MirrorError("SYNC_UNAVAILABLE") from exc
    except (UnicodeError, ValueError, TypeError) as exc:
        raise MirrorError("SYNC_PROTOCOL_INVALID") from exc


def run_once(mirror: PcSharedMirror, bearer: str) -> dict:
    with mirror._lock():
        total, more, pages = 0, False, 0
        for i in range(MAX_PAGES):
            previous = mirror.cursor()
            page = _pull(mirror.origin, bearer, previous)
            added, more, _ = mirror.apply_page(page, previous)
            total += added
            pages = i + 1
            if not more:
                break
        return {"received": total, "cursor": mirror.cursor(),
                "more_available": more, "pages": pages}


def main() -> int:
    parser = argparse.ArgumentParser(description="ORUN isolated shared-data PC read mirror")
    parser.add_argument("--db", required=True, help="Dedicated private SQLite mirror file")
    parser.add_argument("--site", required=True)
    parser.add_argument("--installation", required=True)
    parser.add_argument("--url", required=True, help="HTTPS origin; HTTP only for 127.0.0.1")
    actions = parser.add_mutually_exclusive_group(required=True)
    actions.add_argument("--once", action="store_true",
                         help="One authenticated pull; no daemon or upload")
    actions.add_argument("--list", action="store_true",
                         help="Print only locally cached zone/group/target JSON")
    args = parser.parse_args()
    try:
        mirror = PcSharedMirror(args.db, args.site, args.installation, args.url)
        if args.once:
            result = run_once(mirror, os.environ.get("ORUN_SHARED_SYNC_TOKEN", ""))
        else:
            result = mirror.snapshot()
        print(json.dumps(result, ensure_ascii=False, sort_keys=True))
        return 0
    except (MirrorError, sqlite3.Error) as exc:
        print(exc if isinstance(exc, MirrorError) else "SYNC_MIRROR_DB_FAILURE",
              file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
