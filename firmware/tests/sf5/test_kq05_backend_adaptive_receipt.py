#!/usr/bin/env python3
"""KQ05 adaptive 0x09 outbox: real AEAD verified ledger + signed HMAC.

No product HTTP, RF transmission, on-device Flash, or production credentials.
"""
from __future__ import annotations

from pathlib import Path
import sqlite3
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "backend/candidates"))
sys.path.insert(0, str(ROOT / "firmware/tests/sf5"))

import kq05_backend_batch_receipt as batch
import kq05_backend_receipt_outbox as queue
from kq05_backend_adaptive_receipt import AdaptiveBackendReceiptStager as Stager, State
import product_secure_verified_ledger as ledger
from test_kq05_backend_batch_receipt import rows
from test_sf5h_backend_verified_ledger import CRED, DEVICE, EPOCH, keys, reencrypt, public_fixture


def prepare(folder: str, *, cap: int = 4096):
    store = ledger.VerifiedLocalLedger(Path(folder) / "verified.db", keys)
    outbox = queue.BackendReceiptOutbox(batch.BackendBatchIssuer(store),
                                       max_items=cap)
    return store, outbox


def receipt(outbox, inc, expected: tuple[int, ...]):
    scan = outbox.oldest(8)
    assert scan.state == queue.ReadState.READY
    assert batch.verify_for_tracker(scan.items[-1].wire, CRED,
                                    DEVICE, inc, EPOCH) == expected


def test_dense_five_and_no_stale_restage():
    with tempfile.TemporaryDirectory() as d:
        store, outbox = prepare(d)
        frames = rows(7)
        inc = ledger.open_product_frame(frames[0], keys).incarnation
        stager = Stager(outbox, DEVICE, inc, EPOCH, trusted_report_seconds=60)
        assert stager.poll(1000).state == State.EMPTY
        for frame in frames[:4]:
            assert store.ingest(frame).state == ledger.LedgerState.VERIFIED_STORED
        assert stager.poll(1000).state == State.WAITING
        assert stager.poll(120999).state == State.WAITING
        assert store.ingest(frames[4]).state == ledger.LedgerState.VERIFIED_STORED
        staged = stager.poll(121000)
        assert staged.state == State.STORED
        assert staged.sequences == (101, 102, 103, 104, 105)
        receipt(outbox, inc, staged.sequences)
        assert stager.poll(121001).state == State.EMPTY
        # A reboot must not create another pending receipt for the same five.
        restarted = Stager(queue.BackendReceiptOutbox(
            batch.BackendBatchIssuer(
                ledger.VerifiedLocalLedger(Path(d) / "verified.db", keys))),
            DEVICE, inc, EPOCH, trusted_report_seconds=60)
        assert restarted.poll(121002).state == State.EMPTY
        for frame in frames[5:]:
            assert store.ingest(frame).state == ledger.LedgerState.VERIFIED_STORED
        assert restarted.poll(200000).state == State.WAITING
        assert restarted.poll(320000).state == State.STORED
        receipt(outbox, inc, (106, 107))
        assert len(outbox.oldest(8).items) == 2


def test_gap_and_partial_timer_wrap():
    with tempfile.TemporaryDirectory() as d:
        store, outbox = prepare(d)
        frames = rows(5)
        inc = ledger.open_product_frame(frames[0], keys).incarnation
        for i in (0, 1, 3, 4):
            assert store.ingest(frames[i]).state == ledger.LedgerState.VERIFIED_STORED
        stager = Stager(outbox, DEVICE, inc, EPOCH, trusted_report_seconds=900)
        assert stager.poll(0xFFFFFF00).state == State.WAITING
        assert stager.poll((0xFFFFFF00 + 119999) & 0xFFFFFFFF).state == State.WAITING
        staged = stager.poll((0xFFFFFF00 + 120000) & 0xFFFFFFFF)
        assert staged.state == State.STORED
        assert staged.sequences == (101, 102, 104, 105)
        receipt(outbox, inc, staged.sequences)
        assert outbox.oldest(8).items[0].wire[3] == 4
        # Later arrival of missing #103 can be ACKed alone, without
        # treating the old subset as a cumulative 105 high-water mark.
        assert store.ingest(frames[2]).state == ledger.LedgerState.VERIFIED_STORED
        assert stager.poll(123000).state == State.WAITING
        assert stager.poll(243000).sequences == (103,)


def test_sparse_immediate_no_five_wait():
    with tempfile.TemporaryDirectory() as d:
        store, outbox = prepare(d)
        frames = rows(3)
        inc = ledger.open_product_frame(frames[0], keys).incarnation
        stager = Stager(outbox, DEVICE, inc, EPOCH, trusted_report_seconds=3600)
        for index, frame in enumerate(frames):
            assert store.ingest(frame).state == ledger.LedgerState.VERIFIED_STORED
            result = stager.poll(5000 + index)
            assert result.state == State.STORED
            assert result.sequences == (101 + index,)
        assert len(outbox.oldest(8).items) == 3
        assert stager.poll(6000).state == State.EMPTY


def test_corruption_conflict_full_fail_closed():
    with tempfile.TemporaryDirectory() as d:
        store, outbox = prepare(d, cap=1)
        frames = rows(3)
        inc = ledger.open_product_frame(frames[0], keys).incarnation
        stager = Stager(outbox, DEVICE, inc, EPOCH, trusted_report_seconds=3600)
        assert store.ingest(frames[0]).state == ledger.LedgerState.VERIFIED_STORED
        assert stager.poll(1000).state == State.STORED
        assert store.ingest(frames[1]).state == ledger.LedgerState.VERIFIED_STORED
        assert stager.poll(2000).state == State.FULL
        assert len(outbox.oldest(8).items) == 1
        with sqlite3.connect(store.path) as db:
            wire = bytearray(db.execute(
                "SELECT wire FROM kq05_receipt_outbox WHERE id=1").fetchone()[0])
            wire[-1] ^= 0x01
            db.execute("UPDATE kq05_receipt_outbox SET wire=? WHERE id=1",
                       (sqlite3.Binary(bytes(wire)),))
        assert stager.poll(3000).state == State.REFUSED
        assert stager.poll(True).state == State.INVALID

    with tempfile.TemporaryDirectory() as d:
        store, outbox = prepare(d)
        frames = rows(2)
        inc = ledger.open_product_frame(frames[0], keys).incarnation
        assert store.ingest(frames[0]).state == ledger.LedgerState.VERIFIED_STORED
        opened = ledger.open_product_frame(frames[0], keys)
        changed = bytearray(opened.plaintext)
        changed[56:58] = (3700).to_bytes(2, "big")
        alternative = reencrypt(public_fixture("kPeriodicFrameHex"), 9991,
                                bytes(changed), flags=0)
        assert store.ingest(alternative).state == ledger.LedgerState.CONTENT_CONFLICT
        assert store.ingest(frames[1]).state == ledger.LedgerState.VERIFIED_STORED
        s = Stager(outbox, DEVICE, inc, EPOCH, trusted_report_seconds=3600)
        only_second = s.poll(0)
        assert only_second.state == State.STORED
        assert only_second.sequences == (102,)
        assert s.poll(1000).state == State.EMPTY
        # Contested 101 never gets a receipt.
        receipt(outbox, inc, (102,))


def main():
    test_dense_five_and_no_stale_restage()
    test_gap_and_partial_timer_wrap()
    test_sparse_immediate_no_five_wait()
    test_corruption_conflict_full_fail_closed()
    print("KQ05 adaptive verified Backend 0x09: sparse single, dense five, gap, "
          "reboot, corrupt outbox, conflicts, full: PASS (HOST ONLY)")


if __name__ == "__main__":
    main()
