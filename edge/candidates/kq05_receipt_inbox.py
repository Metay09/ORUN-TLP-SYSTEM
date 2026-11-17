"""KQ05 LAB: Edge-side durable inbox for authenticated Backend receipt pages.

No Gateway upload, RF downlink, receipt consumption, Tracker release or
automatic deletion. Page MAC is authenticated with an Edge-specific transport
key; the embedded Backend->Tracker HMAC can ONLY be checked by Tracker.
Do not use as a live Edge service without enrollment/TLS/revocation review.
"""
from __future__ import annotations

from contextlib import closing
from dataclasses import dataclass
from enum import Enum, auto
from hashlib import sha256
import hmac
from pathlib import Path
import re
import sqlite3
import struct

from kq05_receipt_edge_transport import EdgeRegistration, verify_response

_MAX_CURSOR = 9223372036854775807
_HEX = re.compile(r"[0-9a-f]{88,144}\Z", re.ASCII)
_DOMAIN = b"ORUN-KQ05-EDGE-INBOX-ROW-V1\x00"


class StoreState(Enum):
    STORED = auto()        # persisted on THIS Edge, not Gateway/Tracker
    DUPLICATE = auto()
    EMPTY = auto()
    INVALID = auto()
    FULL = auto()
    REFUSED = auto()
    UNAVAILABLE = auto()


@dataclass(frozen=True)
class Receipt:
    row_id: int
    wire: bytes


@dataclass(frozen=True)
class Scan:
    state: StoreState
    cursor: int = 0
    items: tuple[Receipt, ...] = ()


class EdgeReceiptInbox:
    """One installation-owned SQLite inbox with atomic page/cursor commits."""

    def __init__(self, path: str | Path, registration: EdgeRegistration,
                 *, max_items: int = 4096):
        name = str(path)
        if (not name or name == ":memory:" or name.startswith("file:") or
                not isinstance(registration, EdgeRegistration) or
                type(max_items) is not int or not 1 <= max_items <= 4096):
            raise ValueError("invalid durable Edge inbox")
        self.path = name
        self.registration = registration
        self.max_items = max_items
        with closing(self._conn()) as db:
            if db.execute("PRAGMA journal_mode=WAL").fetchone()[0].lower() != "wal":
                raise RuntimeError("WAL required")
            db.execute("BEGIN IMMEDIATE")
            try:
                db.execute(
                    "CREATE TABLE IF NOT EXISTS kq05_edge_receipt_meta ("
                    " singleton INTEGER PRIMARY KEY CHECK(singleton=1),"
                    " edge TEXT NOT NULL, cursor INTEGER NOT NULL CHECK(cursor>=0))"
                )
                db.execute(
                    "CREATE TABLE IF NOT EXISTS kq05_edge_receipt_items ("
                    " id INTEGER PRIMARY KEY CHECK(id>0),"
                    " wire BLOB NOT NULL CHECK(length(wire) BETWEEN 44 AND 72),"
                    " tag BLOB NOT NULL CHECK(length(tag)=32))"
                )
                prior = db.execute(
                    "SELECT edge,cursor FROM kq05_edge_receipt_meta WHERE singleton=1"
                ).fetchone()
                if prior is None:
                    db.execute("INSERT INTO kq05_edge_receipt_meta VALUES(1,?,0)",
                               (registration.edge_id,))
                elif (prior[0] != registration.edge_id or
                      type(prior[1]) is not int or not 0 <= prior[1] <= _MAX_CURSOR):
                    raise ValueError("foreign or corrupt Edge inbox")
                db.execute("COMMIT")
            except Exception:
                if db.in_transaction:
                    db.execute("ROLLBACK")
                raise

    def _conn(self):
        db = sqlite3.connect(self.path, isolation_level=None, timeout=1.0)
        db.execute("PRAGMA busy_timeout=1000")
        db.execute("PRAGMA synchronous=FULL")
        return db

    def _seal(self, row_id: int, wire: bytes) -> bytes:
        return hmac.new(
            self.registration.transport_key,
            _DOMAIN + struct.pack(">Q", row_id) + wire, sha256).digest()

    def _wire_valid(self, wire: bytes) -> bool:
        if (type(wire) is not bytes or not 44 <= len(wire) <= 72 or
                wire[0:3] != b"\x02\x09\x00" or not 1 <= wire[3] <= 8 or
                len(wire) != 40 + 4 * wire[3] or
                wire[4:12] not in self.registration.allowed_devices or
                not any(wire[12:20]) or wire[20:24] == b"\xff" * 4):
            return False
        count = wire[3]
        seq = struct.unpack(">" + "I" * count, wire[24:24 + 4 * count])
        return all(x > 0 for x in seq) and all(a < b for a, b in zip(seq, seq[1:]))

    def _read_all(self, db: sqlite3.Connection) -> Scan:
        meta = db.execute(
            "SELECT edge,cursor FROM kq05_edge_receipt_meta WHERE singleton=1"
        ).fetchall()
        if (len(meta) != 1 or meta[0][0] != self.registration.edge_id or
                type(meta[0][1]) is not int or
                not 0 <= meta[0][1] <= _MAX_CURSOR):
            return Scan(StoreState.REFUSED)
        cursor = meta[0][1]
        items = []
        previous = 0
        for row_id, wire, tag in db.execute(
                "SELECT id,wire,tag FROM kq05_edge_receipt_items ORDER BY id"):
            if (len(items) >= self.max_items or type(row_id) is not int or
                    row_id <= previous or row_id > cursor or
                    not self._wire_valid(wire) or type(tag) is not bytes or
                    not hmac.compare_digest(tag, self._seal(row_id, wire))):
                return Scan(StoreState.REFUSED)
            previous = row_id
            items.append(Receipt(row_id, wire))
        return Scan(StoreState.STORED if items else StoreState.EMPTY,
                    cursor, tuple(items))

    def scan(self) -> Scan:
        try:
            with closing(self._conn()) as db:
                db.execute("BEGIN")
                read = self._read_all(db)
                db.execute("ROLLBACK")
                return read
        except (sqlite3.Error, OSError, ValueError, struct.error):
            return Scan(StoreState.UNAVAILABLE)

    def accept_page(self, request: object, response: object) -> StoreState:
        if (type(request) is not dict or type(response) is not dict or
                not verify_response(self.registration, request, response)):
            return StoreState.INVALID
        after, cursor, raw_items = response["after"], response["cursor"], response["items"]
        if (type(after) is not int or not 0 <= after <= _MAX_CURSOR or
                type(cursor) is not int or not after <= cursor <= _MAX_CURSOR or
                not isinstance(raw_items, list)):
            return StoreState.INVALID
        checked = []
        previous = after
        for obj in raw_items:
            if (type(obj) is not dict or set(obj) != {"id", "wire"} or
                    type(obj["id"]) is not int or
                    not previous < obj["id"] <= _MAX_CURSOR or
                    type(obj["wire"]) is not str or len(obj["wire"]) % 2 != 0 or
                    not _HEX.fullmatch(obj["wire"])):
                return StoreState.REFUSED
            wire = bytes.fromhex(obj["wire"])
            if not self._wire_valid(wire):
                return StoreState.REFUSED
            checked.append(Receipt(obj["id"], wire))
            previous = obj["id"]
        if (cursor != (checked[-1].row_id if checked else after) or
                not checked and response["more"]):
            return StoreState.REFUSED
        try:
            with closing(self._conn()) as db:
                db.execute("BEGIN IMMEDIATE")
                current = self._read_all(db)
                if current.state == StoreState.REFUSED:
                    db.execute("ROLLBACK")
                    return StoreState.REFUSED
                existing = {item.row_id: item.wire for item in current.items}
                if current.cursor != after:
                    # A repeated response is harmless only when identical to
                    # the *entire* already committed page, never a stale cursor.
                    match = (current.cursor == cursor and
                             all(existing.get(x.row_id) == x.wire for x in checked))
                    db.execute("ROLLBACK")
                    return StoreState.DUPLICATE if match else StoreState.REFUSED
                if any((item.row_id in existing and existing[item.row_id] != item.wire)
                       for item in checked):
                    db.execute("ROLLBACK")
                    return StoreState.REFUSED
                fresh = [x for x in checked if x.row_id not in existing]
                if len(existing) + len(fresh) > self.max_items:
                    db.execute("ROLLBACK")
                    return StoreState.FULL
                for item in fresh:
                    db.execute(
                        "INSERT INTO kq05_edge_receipt_items (id,wire,tag)"
                        " VALUES (?,?,?)",
                        (item.row_id, item.wire, self._seal(item.row_id, item.wire)))
                db.execute(
                    "UPDATE kq05_edge_receipt_meta SET cursor=? WHERE singleton=1",
                    (cursor,))
                db.execute("COMMIT")
            # The independent post-COMMIT readback does not declare delivery.
            verified = self.scan()
            if (verified.state in (StoreState.UNAVAILABLE, StoreState.REFUSED) or
                    verified.cursor != cursor):
                return StoreState.UNAVAILABLE
            matched = {item.row_id: item.wire for item in verified.items}
            if any(matched.get(x.row_id) != x.wire for x in checked):
                return StoreState.UNAVAILABLE
            return StoreState.STORED if checked else StoreState.EMPTY
        except (sqlite3.Error, OSError, ValueError, struct.error):
            return StoreState.UNAVAILABLE

    # Deliberately NO pop(), ACK(), delete(), reclaim(), or RF send().
