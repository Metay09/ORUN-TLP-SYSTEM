"""KQ05 LAB ONLY: bounded, adaptive Backend-DURABLE 0x09 receipt staging.

This adapter reads ONLY the verified Backend ledger, filters PERIODIC identity
candidates not already present in the persistent outbox, and asks the existing
authenticated BackendReceiptOutbox.enqueue() to issue each selected receipt.
It never signs on Gateway, releases Tracker Flash, sends RF, or retires queues.

A trusted Backend report profile (not LoRa bytes) may choose immediate single
receipts for sparse >=3600s reporting. Dense traffic groups up to five verified
records; a 120s first-pending deadline flushes any 1..4 remainder. These
values are provisional policy until real RF/energy qualification.
"""
from __future__ import annotations

from contextlib import closing
from dataclasses import dataclass
from enum import Enum, auto
import sqlite3
import struct

from kq05_backend_receipt_outbox import (
    BackendReceiptOutbox, PutState, ReadState,
)


class State(Enum):
    STORED = auto()
    DUPLICATE = auto()
    WAITING = auto()
    EMPTY = auto()
    FULL = auto()
    REFUSED = auto()
    UNAVAILABLE = auto()
    INVALID = auto()


@dataclass(frozen=True)
class Result:
    state: State
    sequences: tuple[int, ...] = ()
    row_id: int | None = None


class AdaptiveBackendReceiptStager:
    """Cooperative Backend-only scheduler; caller supplies monotonic milliseconds.

    This object contains only a volatile *timer*, no delivery/release truth.
    Reboot cannot delete already durable Backend/Edge/Gateway queue records.
    Scope is pinned at construction, preventing mixed-Tracker ACK groups.
    """

    MAX_BATCH = 5
    MAX_WAIT_MS = 120_000

    def __init__(self, outbox: BackendReceiptOutbox,
                 device: bytes, incarnation: bytes, epoch: int,
                 *, trusted_report_seconds: int):
        if (not isinstance(outbox, BackendReceiptOutbox) or
                type(device) is not bytes or len(device) != 8 or not any(device) or
                type(incarnation) is not bytes or len(incarnation) != 8 or
                not any(incarnation) or
                type(epoch) is not int or not 0 <= epoch < 0xFFFFFFFF or
                type(trusted_report_seconds) is not int or
                not 1 <= trusted_report_seconds <= 86400):
            raise ValueError("trusted Backend receipt scope/report profile required")
        self.outbox = outbox
        self.device = device
        self.incarnation = incarnation
        self.epoch = epoch
        self.sparse = trusted_report_seconds >= 3600
        self._first_pending_ms: int | None = None

    def _verified_unstaged(self) -> tuple[int, ...] | None:
        """Return candidates only; enqueue() still redoes AEAD + HMAC under
        the same SQLite writer reservation before granting durable staging.
        """
        # _scan rechecks ALL persisted outbox rows against AEAD and HMAC,
        # including rows outside its returned one-item page.
        scanned = self.outbox.oldest(1)
        if scanned.state not in (ReadState.READY, ReadState.EMPTY):
            return None
        with closing(self.outbox._conn()) as db:
            db.execute("BEGIN")
            try:
                covered: set[int] = set()
                for (blob,) in db.execute(
                    "SELECT sequences FROM kq05_receipt_outbox "
                    "WHERE device=? AND incarnation=? AND epoch=?",
                    (self.device, self.incarnation, self.epoch),
                ):
                    if type(blob) is not bytes or len(blob) not in range(4, 33, 4):
                        return None
                    covered.update(struct.unpack(">" + "I" * (len(blob) // 4), blob))

                conflicts = {
                    row[0] for row in db.execute(
                        "SELECT DISTINCT sequence FROM verified_conflicts "
                        "WHERE device_id=? AND incarnation=?",
                        (self.device, self.incarnation),
                    )
                }
                result: list[int] = []
                for seq, frame in db.execute(
                    "SELECT sequence,exact_first_frame FROM verified_observations "
                    "WHERE device_id=? AND incarnation=? AND family=1 "
                    "ORDER BY sequence ASC",
                    (self.device, self.incarnation),
                ):
                    if (type(seq) is not int or not 0 < seq <= 0xFFFFFFFF or
                            type(frame) is not bytes or len(frame) != 100):
                        return None
                    # Match trusted actual AEAD header epoch; never group
                    # rows from another credential epoch under this key.
                    if int.from_bytes(frame[16:20], "big") != self.epoch:
                        continue
                    if seq not in covered and seq not in conflicts:
                        result.append(seq)
                db.execute("COMMIT")
                return tuple(result)
            finally:
                if db.in_transaction:
                    db.execute("ROLLBACK")

    def poll(self, now_ms: int) -> Result:
        if type(now_ms) is not int or not 0 <= now_ms <= 0xFFFFFFFF:
            return Result(State.INVALID)
        try:
            queued = self.outbox.oldest(1)
            if queued.state == ReadState.REFUSED:
                self._first_pending_ms = None
                return Result(State.REFUSED)
            if queued.state not in (ReadState.READY, ReadState.EMPTY):
                return Result(State.UNAVAILABLE)
            pending = self._verified_unstaged()
            if pending is None:
                return Result(State.REFUSED)
            if not pending:
                self._first_pending_ms = None
                return Result(State.EMPTY)

            # Only Backend-trusted reporting policy enables sparse immediate
            # ACK. No "must collect five" wait for an hourly device.
            ready = self.sparse or len(pending) >= self.MAX_BATCH
            if not ready:
                if self._first_pending_ms is None:
                    self._first_pending_ms = now_ms
                    return Result(State.WAITING)
                elapsed = (now_ms - self._first_pending_ms) & 0xFFFFFFFF
                ready = elapsed >= self.MAX_WAIT_MS
            if not ready:
                return Result(State.WAITING)

            selected = pending[:1] if self.sparse else pending[:self.MAX_BATCH]
            outcome = self.outbox.enqueue(
                self.device, self.incarnation, self.epoch, selected)
            if outcome.state in (PutState.STORED, PutState.DUPLICATE):
                self._first_pending_ms = None
                return Result(
                    State.STORED if outcome.state == PutState.STORED
                    else State.DUPLICATE,
                    selected, outcome.row_id)
            if outcome.state == PutState.FULL:
                return Result(State.FULL)
            if outcome.state == PutState.REFUSED:
                return Result(State.REFUSED)
            if outcome.state == PutState.NOT_DURABLE:
                # Verified data was invalidated between selection/issue.
                # Never claim an ACK or mark any record delivered.
                return Result(State.REFUSED)
            return Result(State.UNAVAILABLE)
        except (sqlite3.Error, OSError, ValueError, TypeError, struct.error):
            return Result(State.UNAVAILABLE)
