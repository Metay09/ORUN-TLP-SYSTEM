"""KQ05 LAB ONLY: bounded Gateway durable staging for opaque Backend receipts.

No firmware boot path, BLE/USB handler, radio TX, Tracker Flash release,
legacy single-ACK replacement, or automatic deletion is implemented.
A Gateway cannot verify Backend receipt HMAC; the Tracker MUST independently
authenticate every receipt before any selective durable Flash release.
"""
from __future__ import annotations

from contextlib import closing
from dataclasses import dataclass
from enum import Enum, auto
import hmac
from hashlib import sha256
from pathlib import Path
import sqlite3
import struct

from kq05_edge_gateway_offer import (
    GatewayLinkPermit, receipt_shape, verify_offer,
)

_DOMAIN = b"ORUN-KQ05-GATEWAY-STAGED-v1\x00"
_MAX_ID = 9223372036854775807


class StageState(Enum):
    STAGED = auto()     # persisted on Gateway host only
    DUPLICATE = auto()  # same Edge row and EXACT same opaque receipt
    FULL = auto()
    REFUSED = auto()    # tamper, wrong scope, conflicting ID, corrupt DB
    UNAVAILABLE = auto()


@dataclass(frozen=True)
class StageResult:
    state: StageState
    local_id: int | None = None


@dataclass(frozen=True)
class HeldReceipt:
    local_id: int
    source_row: int
    wire: bytes


@dataclass(frozen=True)
class ScanResult:
    state: StageState
    items: tuple[HeldReceipt, ...] = ()
    more: bool = False


class GatewayReceiptStage:
    """No dequeue API: staging is NOT Tracker delivery or backend ACK."""

    def __init__(self, path: str | Path, permit: GatewayLinkPermit,
                 *, capacity: int = 4096):
        name = str(path)
        if (not name or name == ":memory:" or name.startswith("file:") or
                not isinstance(permit, GatewayLinkPermit) or
                type(capacity) is not int or not 1 <= capacity <= 4096):
            raise ValueError("invalid Gateway host staging")
        self.path = name
        self.permit = permit
        self.capacity = capacity
        with closing(self._conn()) as db:
            if db.execute("PRAGMA journal_mode=WAL").fetchone()[0].lower() != "wal":
                raise RuntimeError("Gateway host staging requires WAL")
            db.execute("BEGIN IMMEDIATE")
            try:
                db.execute(
                    "CREATE TABLE IF NOT EXISTS kq05_gateway_meta ("
                    " singleton INTEGER PRIMARY KEY CHECK(singleton=1),"
                    " edge TEXT NOT NULL, gateway BLOB NOT NULL,"
                    " key_hash BLOB NOT NULL CHECK(length(key_hash)=32))")
                db.execute(
                    "CREATE TABLE IF NOT EXISTS kq05_gateway_receipts ("
                    " id INTEGER PRIMARY KEY AUTOINCREMENT,"
                    " source_row INTEGER NOT NULL UNIQUE CHECK(source_row>0),"
                    " wire BLOB NOT NULL CHECK(length(wire) BETWEEN 44 AND 72),"
                    " tag BLOB NOT NULL CHECK(length(tag)=32))")
                found = db.execute(
                    "SELECT edge,gateway,key_hash FROM kq05_gateway_meta "
                    "WHERE singleton=1").fetchone()
                expected = self._identity()
                if found is None:
                    db.execute("INSERT INTO kq05_gateway_meta VALUES (1,?,?,?)",
                               expected)
                elif found != expected:
                    raise ValueError("foreign Gateway/Edge/credential staging DB")
                db.execute("COMMIT")
            except Exception:
                if db.in_transaction:
                    db.execute("ROLLBACK")
                raise

    def _identity(self):
        return (self.permit.edge_id, self.permit.gateway_id,
                sha256(self.permit.key).digest())

    def _conn(self):
        db = sqlite3.connect(self.path, timeout=1.0, isolation_level=None)
        db.execute("PRAGMA busy_timeout=1000")
        db.execute("PRAGMA synchronous=FULL")
        return db

    def _seal(self, source_row: int, wire: bytes) -> bytes:
        return hmac.new(
            self.permit.key,
            _DOMAIN + self.permit.edge_id.encode("ascii") + b"\x00" +
            self.permit.gateway_id + struct.pack(">Q", source_row) + wire,
            sha256).digest()

    def _read_full(self, db) -> ScanResult:
        meta = db.execute(
            "SELECT edge,gateway,key_hash FROM kq05_gateway_meta"
        ).fetchall()
        if len(meta) != 1 or meta[0] != self._identity():
            return ScanResult(StageState.REFUSED)
        held = []
        last_id = 0
        last_source = set()
        for local_id, source_row, wire, tag in db.execute(
                "SELECT id,source_row,wire,tag FROM kq05_gateway_receipts "
                "ORDER BY id ASC"):
            if (len(held) >= self.capacity or
                    type(local_id) is not int or
                    not last_id < local_id <= _MAX_ID or
                    type(source_row) is not int or
                    not 1 <= source_row <= _MAX_ID or source_row in last_source or
                    not receipt_shape(wire, self.permit) or
                    type(tag) is not bytes or not
                    hmac.compare_digest(tag, self._seal(source_row, wire))):
                return ScanResult(StageState.REFUSED)
            last_id = local_id
            last_source.add(source_row)
            held.append(HeldReceipt(local_id, source_row, wire))
        return ScanResult(StageState.STAGED, tuple(held))

    def scan(self, limit: int = 8, after_id: int = 0) -> ScanResult:
        if (type(limit) is not int or not 1 <= limit <= 32 or
                type(after_id) is not int or not 0 <= after_id <= _MAX_ID):
            return ScanResult(StageState.REFUSED)
        try:
            with closing(self._conn()) as db:
                db.execute("BEGIN")
                all_records = self._read_full(db)
                db.execute("ROLLBACK")
                if all_records.state != StageState.STAGED:
                    return all_records
                candidates = [x for x in all_records.items if x.local_id > after_id]
                return ScanResult(
                    StageState.STAGED, tuple(candidates[:limit]),
                    len(candidates) > limit)
        except (sqlite3.Error, OSError, ValueError, struct.error):
            return ScanResult(StageState.UNAVAILABLE)

    def accept(self, data: object) -> StageResult:
        offer = verify_offer(data, self.permit)
        if offer is None:
            return StageResult(StageState.REFUSED)
        try:
            with closing(self._conn()) as db:
                db.execute("BEGIN IMMEDIATE")
                scanned = self._read_full(db)
                if scanned.state != StageState.STAGED:
                    db.execute("ROLLBACK")
                    return StageResult(StageState.REFUSED)
                previous = next(
                    (x for x in scanned.items if x.source_row == offer.source_row),
                    None)
                if previous is not None:
                    db.execute("ROLLBACK")
                    if hmac.compare_digest(previous.wire, offer.wire):
                        return StageResult(StageState.DUPLICATE, previous.local_id)
                    return StageResult(StageState.REFUSED)
                if len(scanned.items) >= self.capacity:
                    db.execute("ROLLBACK")
                    return StageResult(StageState.FULL)
                row_id = db.execute(
                    "INSERT INTO kq05_gateway_receipts(source_row,wire,tag)"
                    " VALUES(?,?,?)",
                    (offer.source_row, offer.wire,
                     self._seal(offer.source_row, offer.wire))).lastrowid
                db.execute("COMMIT")
            # Successful independent SQLite readback; not evidence of RF TX.
            check = self.scan(limit=32)
            if (check.state != StageState.STAGED or
                    not any(x.local_id == row_id and
                            x.source_row == offer.source_row and
                            hmac.compare_digest(x.wire, offer.wire)
                            for x in check.items)):
                # Do not assert failure for records beyond page 1: inspect
                # exact committed row after the complete integrity scan.
                if check.state != StageState.STAGED:
                    return StageResult(StageState.UNAVAILABLE)
                with closing(self._conn()) as db:
                    exact = db.execute(
                        "SELECT source_row,wire,tag FROM kq05_gateway_receipts"
                        " WHERE id=?", (row_id,)).fetchone()
                    if (exact is None or exact[0] != offer.source_row or
                            exact[1] != offer.wire or
                            not hmac.compare_digest(
                                exact[2], self._seal(offer.source_row, offer.wire))):
                        return StageResult(StageState.UNAVAILABLE)
            return StageResult(StageState.STAGED, row_id)
        except (sqlite3.Error, OSError, ValueError, struct.error):
            return StageResult(StageState.UNAVAILABLE)

    # Intentionally no pop(), delete(), reclaim(), RF transmit, or release().
