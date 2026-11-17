"""KQ05 LAB: persistent Backend receipt staging, NOT a delivery/erase authority.

Shares the verified Backend ledger's SQLite database. Enqueue holds SQLite's
writer reservation across independent AEAD/HMAC issuance and insert, so no
concurrent ledger writer can insert an observation conflict in that interval.
This is not an authenticated HTTP service, Gateway queue, RF downlink, or
proof of delivery to a Tracker. No delete, sent, release, or ACK API exists.
"""
from __future__ import annotations

from contextlib import closing
from dataclasses import dataclass
from enum import Enum, auto
import hmac
import sqlite3
import struct
from typing import Sequence

from kq05_backend_batch_receipt import BackendBatchIssuer, State as IssueState


class PutState(Enum):
    STORED = auto()       # in the local Backend's SQLite outbox only
    DUPLICATE = auto()
    INVALID = auto()
    NOT_DURABLE = auto()
    FULL = auto()
    REFUSED = auto()      # conflicting/corrupt persisted outbox
    UNAVAILABLE = auto()


class ReadState(Enum):
    READY = auto()
    EMPTY = auto()
    INVALID = auto()
    REFUSED = auto()
    UNAVAILABLE = auto()


@dataclass(frozen=True)
class PutResult:
    state: PutState
    row_id: int | None = None


@dataclass(frozen=True)
class Item:
    row_id: int
    wire: bytes  # 44..72B authenticated Backend receipt; NOT transport authority


@dataclass(frozen=True)
class ReadResult:
    state: ReadState
    items: tuple[Item, ...] = ()


class BackendReceiptOutbox:
    """Bounded, fail-closed, persistent *read-only dispatch candidate*.

    Repeated reads return the same items until a future authenticated Edge
    transfer protocol explicitly defines a safe retirement authority. Do NOT
    interpret reading or passing this data to another process as delivery.
    """

    def __init__(self, issuer: BackendBatchIssuer, *, max_items: int = 4096):
        if not isinstance(issuer, BackendBatchIssuer):
            raise ValueError("verified Backend issuer required")
        if type(max_items) is not int or not 1 <= max_items <= 4096:
            raise ValueError("invalid bounded outbox capacity")
        self.issuer = issuer
        self.max_items = max_items
        with closing(self._conn()) as db:
            db.execute("BEGIN IMMEDIATE")
            try:
                db.execute(
                    "CREATE TABLE IF NOT EXISTS kq05_receipt_outbox ("
                    " id INTEGER PRIMARY KEY AUTOINCREMENT,"
                    " device BLOB NOT NULL CHECK(length(device)=8),"
                    " incarnation BLOB NOT NULL CHECK(length(incarnation)=8),"
                    " epoch INTEGER NOT NULL CHECK(epoch>=0 AND epoch<4294967295),"
                    " sequences BLOB NOT NULL CHECK(length(sequences) BETWEEN 4 AND 32"
                    " AND length(sequences)%4=0),"
                    " wire BLOB NOT NULL CHECK(length(wire)=40+length(sequences)),"
                    " UNIQUE(device,incarnation,epoch,sequences))"
                )
                db.execute("COMMIT")
            except Exception:
                db.execute("ROLLBACK")
                raise

    def _conn(self) -> sqlite3.Connection:
        # Ledger and outbox must share a single SQLite WAL lock domain.
        return self.issuer.ledger._conn()

    @staticmethod
    def _sequences_blob(sequences: Sequence[int]) -> bytes | None:
        if (type(sequences) not in (tuple, list) or
                not 1 <= len(sequences) <= 8 or
                any(type(v) is not int or not 0 < v <= 0xFFFFFFFF for v in sequences) or
                any(a >= b for a, b in zip(sequences, sequences[1:]))):
            return None
        return b"".join(struct.pack(">I", v) for v in sequences)

    def _signed(self, device: bytes, inc: bytes, epoch: int,
                sequences: tuple[int, ...]):
        return self.issuer.issue(device, inc, epoch, sequences)

    def enqueue(self, device: bytes, incarnation: bytes, epoch: int,
                sequences: Sequence[int]) -> PutResult:
        blob = self._sequences_blob(sequences)
        if (blob is None or type(device) is not bytes or len(device) != 8 or
                type(incarnation) is not bytes or len(incarnation) != 8 or
                type(epoch) is not int or not 0 <= epoch < 0xFFFFFFFF):
            return PutResult(PutState.INVALID)
        seq = tuple(sequences)
        try:
            with closing(self._conn()) as db:
                # Blocks concurrent verified ledger writes until durable enqueue.
                db.execute("BEGIN IMMEDIATE")
                try:
                    issued = self._signed(device, incarnation, epoch, seq)
                    if issued.state != IssueState.SIGNED:
                        db.execute("ROLLBACK")
                        return PutResult({
                            IssueState.INVALID: PutState.INVALID,
                            IssueState.NOT_DURABLE: PutState.NOT_DURABLE,
                        }.get(issued.state, PutState.UNAVAILABLE))
                    row = db.execute(
                        "SELECT id,wire FROM kq05_receipt_outbox WHERE"
                        " device=? AND incarnation=? AND epoch=? AND sequences=?",
                        (device, incarnation, epoch, blob),
                    ).fetchone()
                    if row:
                        state = (PutState.DUPLICATE if
                                 type(row[0]) is int and row[0] > 0 and
                                 isinstance(row[1], bytes) and
                                 hmac.compare_digest(row[1], issued.wire)
                                 else PutState.REFUSED)
                        db.execute("ROLLBACK")
                        return PutResult(state, row[0] if state == PutState.DUPLICATE else None)
                    count = db.execute("SELECT count(*) FROM kq05_receipt_outbox").fetchone()[0]
                    if type(count) is not int or count > self.max_items:
                        db.execute("ROLLBACK")
                        return PutResult(PutState.REFUSED)
                    if count == self.max_items:
                        db.execute("ROLLBACK")
                        return PutResult(PutState.FULL)
                    new_id = db.execute(
                        "INSERT INTO kq05_receipt_outbox"
                        " (device,incarnation,epoch,sequences,wire) VALUES(?,?,?,?,?)",
                        (device, incarnation, epoch, blob, issued.wire),
                    ).lastrowid
                    db.execute("COMMIT")
                except Exception:
                    if db.in_transaction:
                        db.execute("ROLLBACK")
                    raise
            # Separate connection independently checks committed bytes.
            with closing(self._conn()) as verify:
                saved = verify.execute(
                    "SELECT device,incarnation,epoch,sequences,wire FROM"
                    " kq05_receipt_outbox WHERE id=?", (new_id,),
                ).fetchone()
                if saved != (device, incarnation, epoch, blob, issued.wire):
                    return PutResult(PutState.UNAVAILABLE)
            return PutResult(PutState.STORED, new_id)
        except (sqlite3.Error, OSError, ValueError):
            return PutResult(PutState.UNAVAILABLE)

    def oldest(self, limit: int = 8) -> ReadResult:
        if type(limit) is not int or not 1 <= limit <= 32:
            return ReadResult(ReadState.INVALID)
        try:
            with closing(self._conn()) as db:
                # Fail the whole scan on any invalid queued receipt, even after
                # requested prefix. Blocking writers prevents live revalidation
                # and queue snapshot from observing different ledger versions.
                db.execute("BEGIN IMMEDIATE")
                items: list[Item] = []
                prev = 0
                count = 0
                for row_id, device, inc, epoch, blob, wire in db.execute(
                    "SELECT id,device,incarnation,epoch,sequences,wire"
                    " FROM kq05_receipt_outbox ORDER BY id ASC"):
                    count += 1
                    if (count > self.max_items or type(row_id) is not int or
                            row_id <= prev or type(device) is not bytes or
                            type(inc) is not bytes or type(epoch) is not int or
                            type(blob) is not bytes or type(wire) is not bytes or
                            len(blob) not in range(4, 33, 4)):
                        return ReadResult(ReadState.REFUSED)
                    prev = row_id
                    seq = struct.unpack(">" + "I" * (len(blob) // 4), blob)
                    if self._sequences_blob(seq) != blob:
                        return ReadResult(ReadState.REFUSED)
                    signed = self._signed(device, inc, epoch, seq)
                    if signed.state != IssueState.SIGNED or not hmac.compare_digest(signed.wire, wire):
                        return ReadResult(ReadState.REFUSED)
                    if len(items) < limit:
                        items.append(Item(row_id, wire))
                db.execute("COMMIT")
                return ReadResult(ReadState.READY if items else ReadState.EMPTY,
                                  tuple(items))
        except (sqlite3.Error, OSError, ValueError, struct.error):
            return ReadResult(ReadState.UNAVAILABLE)
