"""ORUN shared resource ledger, first backend S2 slice (standard library only).

Trusted caller ONLY: an Actor must be supplied by a separately authenticated
enrollment/session boundary (Step 3C). Never build Actor from untrusted HTTP JSON.
This is not a tracker/geofence mutation path and never publishes APPLIED.
"""
from __future__ import annotations

from dataclasses import dataclass
import hashlib
import json
from pathlib import Path
import re
import sqlite3
from typing import FrozenSet

ID = re.compile(r"^[A-Za-z0-9._-]{1,128}$")
KINDS = ("zone", "group")
MAX_OPS = 50


class SyncInvalid(ValueError):
    pass


class SyncForbidden(PermissionError):
    pass


@dataclass(frozen=True)
class Actor:
    site_id: str
    actor_id: str
    installation_id: str
    scopes: FrozenSet[str]


def _identity(actor: Actor) -> None:
    if not isinstance(actor, Actor) or any(
        not isinstance(v, str) or not ID.fullmatch(v)
        for v in (actor.site_id, actor.actor_id, actor.installation_id)
    ) or not isinstance(actor.scopes, frozenset):
        raise SyncForbidden("unverified principal")


def _can(actor: Actor, kind: str, action: str) -> bool:
    return f"{kind}:{action}" in actor.scopes


def _identifier(v: object) -> str:
    if not isinstance(v, str) or not ID.fullmatch(v):
        raise SyncInvalid("invalid identifier")
    return v


def _coords(ring: object) -> list[list[int]]:
    if not isinstance(ring, list) or not 3 <= len(ring) <= 65:
        raise SyncInvalid("zone needs 3..64 vertices")
    p: list[tuple[int, int]] = []
    for point in ring:
        if not isinstance(point, list) or len(point) != 2 or any(
            type(n) is not int for n in point
        ):
            raise SyncInvalid("coordinates must be integer [latitude_e7,longitude_e7]")
        lat, lon = point
        if not (-900_000_000 < lat < 900_000_000 and
                -1_800_000_000 < lon < 1_800_000_000):
            raise SyncInvalid("coordinate out of range")
        p.append((lat, lon))
    if p[0] == p[-1]:
        p.pop()
    n = len(p)
    if not 3 <= n <= 64:
        raise SyncInvalid("zone exceeds 64 effective vertices")
    if (max(q[0] for q in p) - min(q[0] for q in p) > 100_000_000 or
            max(q[1] for q in p) - min(q[1] for q in p) > 100_000_000):
        raise SyncInvalid("polygon span exceeds device limit")
    for i in range(n):
        if p[i] == p[(i + 1) % n]:
            raise SyncInvalid("duplicate adjacent vertices")

    origin = p[0]
    twice = sum((a[1] - origin[1]) * (b[0] - origin[0]) -
                (a[0] - origin[0]) * (b[1] - origin[1])
                for a, b in zip(p, p[1:] + p[:1]))
    if twice == 0:
        raise SyncInvalid("degenerate polygon")

    def cross(a, b, q):
        return (b[1] - a[1]) * (q[0] - a[0]) - (b[0] - a[0]) * (q[1] - a[1])

    def on(a, b, q):
        return cross(a, b, q) == 0 and all(
            min(a[k], b[k]) <= q[k] <= max(a[k], b[k]) for k in (0, 1))

    def intersect(a, b, c, d):
        o1, o2, o3, o4 = cross(a, b, c), cross(a, b, d), cross(c, d, a), cross(c, d, b)
        return ((o1 > 0 > o2 or o1 < 0 < o2) and
                (o3 > 0 > o4 or o3 < 0 < o4)) or any((
                    on(a, b, c), on(a, b, d), on(c, d, a), on(c, d, b)))

    for i in range(n):
        for j in range(i + 1, n):
            if j == i + 1 or (i == 0 and j == n - 1):
                continue
            if intersect(p[i], p[(i + 1) % n], p[j], p[(j + 1) % n]):
                raise SyncInvalid("self intersecting polygon")
    return [list(point) for point in p]


def _payload(kind: str, body: object) -> dict:
    if not isinstance(body, dict):
        raise SyncInvalid("payload must be object")
    if kind == "zone":
        if set(body) != {"name", "active", "ring_e7"}:
            raise SyncInvalid("zone schema")
        name = body["name"]
        if not isinstance(name, str) or not 1 <= len(name.strip()) <= 40:
            raise SyncInvalid("zone name")
        if type(body["active"]) is not bool:
            raise SyncInvalid("active must be boolean")
        return {"name": name.strip(), "active": body["active"],
                "ring_e7": _coords(body["ring_e7"])}
    if kind == "group":
        if set(body) != {"name", "device_ids"}:
            raise SyncInvalid("group schema")
        name = body["name"]
        members = body["device_ids"]
        if not isinstance(name, str) or not 1 <= len(name.strip()) <= 60:
            raise SyncInvalid("group name")
        if not isinstance(members, list) or len(members) > 512:
            raise SyncInvalid("group device IDs")
        for member in members:
            _identifier(member)
        if len(set(members)) != len(members):
            raise SyncInvalid("duplicate device ID")
        # Membership is a local/central selection draft, NOT command authority.
        # Later Device Registry must authorize exact target IDs on apply.
        return {"name": name.strip(), "device_ids": sorted(members)}
    raise SyncInvalid("unsupported resource kind")


def _encode(value: object) -> str:
    return json.dumps(value, ensure_ascii=False, sort_keys=True,
                      separators=(",", ":"), allow_nan=False)


class SharedStore:
    def __init__(self, db_path: str | Path):
        self.db_path = Path(db_path)
        if not self.db_path.parent.is_dir():
            raise SyncInvalid("database parent does not exist")
        with self._connect() as db:
            db.executescript("""
                CREATE TABLE IF NOT EXISTS sync_resources(
                    site_id TEXT NOT NULL, kind TEXT NOT NULL, resource_id TEXT NOT NULL,
                    revision INTEGER NOT NULL, body_json TEXT,
                    deleted INTEGER NOT NULL CHECK(deleted IN (0,1)), sequence INTEGER NOT NULL,
                    PRIMARY KEY(site_id,kind,resource_id));
                CREATE TABLE IF NOT EXISTS sync_changes(
                    sequence INTEGER PRIMARY KEY AUTOINCREMENT, site_id TEXT NOT NULL,
                    kind TEXT NOT NULL, resource_id TEXT NOT NULL, revision INTEGER NOT NULL,
                    body_json TEXT, deleted INTEGER NOT NULL CHECK(deleted IN (0,1)),
                    actor_id TEXT NOT NULL, installation_id TEXT NOT NULL);
                CREATE INDEX IF NOT EXISTS sync_changes_by_site ON sync_changes(site_id,sequence);
                CREATE TABLE IF NOT EXISTS sync_results(
                    site_id TEXT NOT NULL, installation_id TEXT NOT NULL, op_id TEXT NOT NULL,
                    op_hash TEXT NOT NULL, response_json TEXT NOT NULL,
                    PRIMARY KEY(site_id,installation_id,op_id));
            """)

    def _connect(self) -> sqlite3.Connection:
        db = sqlite3.connect(str(self.db_path), timeout=8.0)
        db.execute("PRAGMA busy_timeout=8000")
        db.execute("PRAGMA synchronous=FULL")
        db.row_factory = sqlite3.Row
        return db

    def mutate(self, actor: Actor, ops: list[dict]) -> list[dict]:
        _identity(actor)
        if not isinstance(ops, list) or not 1 <= len(ops) <= MAX_OPS:
            raise SyncInvalid("1..50 operations required")
        results: list[dict] = []
        with self._connect() as db:
            db.execute("BEGIN IMMEDIATE")
            for op in ops:
                if not isinstance(op, dict) or set(op) != {
                    "op_id", "kind", "id", "action", "base_revision", "body"
                }:
                    results.append({"status": "REJECTED", "reason": "operation schema"})
                    continue
                try:
                    op_id = _identifier(op["op_id"])
                    kind = op["kind"]
                    rid = _identifier(op["id"])
                    action = op["action"]
                    base = op["base_revision"]
                    if kind not in KINDS or action not in ("UPSERT", "DELETE") or (
                        type(base) is not int or base < 0 or base > 2**53
                    ):
                        raise SyncInvalid("operation kind, action or revision")
                    if action == "DELETE" and op["body"] is not None:
                        raise SyncInvalid("delete body must be null")
                    if not _can(actor, kind, "write"):
                        # No existence/revision disclosure to unauthorized callers.
                        results.append({"op_id": op_id, "status": "REJECTED", "reason": "permission"})
                        continue
                    body = _payload(kind, op["body"]) if action == "UPSERT" else None
                    normalized = {"op_id": op_id, "kind": kind, "id": rid,
                                  "action": action, "base_revision": base, "body": body}
                    request_hash = hashlib.sha256(_encode(normalized).encode()).hexdigest()
                except (SyncInvalid, TypeError, ValueError):
                    results.append({"status": "REJECTED", "reason": "invalid operation"})
                    continue
                duplicate = db.execute(
                    "SELECT op_hash,response_json FROM sync_results "
                    "WHERE site_id=? AND installation_id=? AND op_id=?",
                    (actor.site_id, actor.installation_id, op_id)).fetchone()
                if duplicate:
                    if duplicate["op_hash"] != request_hash:
                        results.append({"op_id": op_id, "status": "REJECTED", "reason": "op_id reuse"})
                    else:
                        results.append(json.loads(duplicate["response_json"]))
                    continue
                row = db.execute(
                    "SELECT revision,deleted FROM sync_resources "
                    "WHERE site_id=? AND kind=? AND resource_id=?",
                    (actor.site_id, kind, rid)).fetchone()
                revision = int(row["revision"]) if row else 0
                if (base != revision or (row and row["deleted"]) or
                    (action == "DELETE" and row is None)):
                    result = {"op_id": op_id, "status": "CONFLICT", "revision": revision}
                else:
                    next_revision = revision + 1
                    body_json = _encode(body) if body is not None else None
                    cur = db.execute(
                        "INSERT INTO sync_changes "
                        "(site_id,kind,resource_id,revision,body_json,deleted,actor_id,installation_id) "
                        "VALUES(?,?,?,?,?,?,?,?)",
                        (actor.site_id, kind, rid, next_revision, body_json,
                         int(action == "DELETE"), actor.actor_id, actor.installation_id))
                    sequence = cur.lastrowid
                    db.execute(
                        "INSERT INTO sync_resources(site_id,kind,resource_id,revision,body_json,deleted,sequence) "
                        "VALUES(?,?,?,?,?,?,?) "
                        "ON CONFLICT(site_id,kind,resource_id) DO UPDATE SET "
                        "revision=excluded.revision,body_json=excluded.body_json,"
                        "deleted=excluded.deleted,sequence=excluded.sequence",
                        (actor.site_id, kind, rid, next_revision, body_json,
                         int(action == "DELETE"), sequence))
                    result = {"op_id": op_id, "status": "ACCEPTED",
                              "revision": next_revision, "sequence": sequence}
                db.execute("INSERT INTO sync_results VALUES(?,?,?,?,?)",
                           (actor.site_id, actor.installation_id, op_id,
                            request_hash, _encode(result)))
                results.append(result)
        return results

    def pull(self, actor: Actor, cursor: int = 0, limit: int = 100) -> dict:
        _identity(actor)
        if type(cursor) is not int or not 0 <= cursor <= 2**63 - 1:
            raise SyncInvalid("invalid cursor")
        if type(limit) is not int or not 1 <= limit <= 200:
            raise SyncInvalid("invalid limit")
        kinds = [kind for kind in KINDS if _can(actor, kind, "read")]
        if not kinds:
            raise SyncForbidden("no authorized readable resources")
        placeholders = ",".join("?" for _ in kinds)
        with self._connect() as db:
            rows = db.execute(
                "SELECT sequence,kind,resource_id,revision,body_json,deleted "
                "FROM sync_changes WHERE site_id=? AND sequence>? "
                f"AND kind IN ({placeholders}) ORDER BY sequence LIMIT ?",
                (actor.site_id, cursor, *kinds, limit)).fetchall()
        changes = [{"sequence": int(r["sequence"]), "kind": r["kind"],
                    "id": r["resource_id"], "revision": int(r["revision"]),
                    "deleted": bool(r["deleted"]),
                    "body": json.loads(r["body_json"]) if r["body_json"] else None}
                   for r in rows]
        return {"changes": changes,
                "cursor": changes[-1]["sequence"] if changes else cursor,
                "has_more": len(changes) == limit}
