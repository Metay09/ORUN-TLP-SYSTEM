#!/usr/bin/env python3
"""KQ05 host-only Backend receipt staging; no Edge/Gateway/Tracker delivery."""
from pathlib import Path
import sqlite3
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / 'backend' / 'candidates'))
sys.path.insert(0, str(ROOT / 'firmware' / 'tests' / 'sf5'))
import kq05_backend_batch_receipt as batch
import kq05_backend_receipt_outbox as queue
import product_secure_verified_ledger as ledger
from test_kq05_backend_batch_receipt import rows
from test_sf5h_backend_verified_ledger import CRED, DEVICE, EPOCH, keys, public_fixture, reencrypt


def test_outbox_restart_bounds_and_selective_gap():
    with tempfile.TemporaryDirectory() as folder:
        path = Path(folder) / 'verified.db'
        store = ledger.VerifiedLocalLedger(path, keys)
        frames = rows(4)
        for idx in (0, 1, 3):
            assert store.ingest(frames[idx]).state == ledger.LedgerState.VERIFIED_STORED
        inc = ledger.open_product_frame(frames[0], keys).incarnation
        issued = batch.BackendBatchIssuer(store)
        outbox = queue.BackendReceiptOutbox(issued, max_items=2)
        assert outbox.oldest().state == queue.ReadState.EMPTY
        assert outbox.enqueue(DEVICE, inc, EPOCH, (103,)).state == queue.PutState.NOT_DURABLE
        assert outbox.enqueue(DEVICE, inc, EPOCH, (101, 103)).state == queue.PutState.NOT_DURABLE
        assert outbox.enqueue(DEVICE, inc, EPOCH, (102, 101)).state == queue.PutState.INVALID
        assert outbox.enqueue(DEVICE, inc, EPOCH, (101, 101)).state == queue.PutState.INVALID
        assert outbox.enqueue(DEVICE, inc, EPOCH, tuple(range(1, 10))).state == queue.PutState.INVALID
        assert outbox.enqueue(DEVICE, inc, EPOCH + 1, (101,)).state == queue.PutState.UNAVAILABLE
        first = outbox.enqueue(DEVICE, inc, EPOCH, (101, 102))
        assert first.state == queue.PutState.STORED and first.row_id == 1
        assert outbox.enqueue(DEVICE, inc, EPOCH, (101, 102)) == queue.PutResult(queue.PutState.DUPLICATE, 1)
        read = outbox.oldest(1)
        assert read.state == queue.ReadState.READY and len(read.items) == 1
        assert batch.verify_for_tracker(read.items[0].wire, CRED, DEVICE, inc, EPOCH) == (101, 102)
        # A database process restart must neither delete nor mark a receipt delivered.
        restarted = queue.BackendReceiptOutbox(batch.BackendBatchIssuer(
            ledger.VerifiedLocalLedger(path, keys)), max_items=2)
        assert restarted.oldest(1) == read
        assert restarted.enqueue(DEVICE, inc, EPOCH, (104,)).state == queue.PutState.STORED
        assert restarted.enqueue(DEVICE, inc, EPOCH, (101,)).state == queue.PutState.FULL
        assert len(restarted.oldest(2).items) == 2
        assert restarted.oldest(0).state == queue.ReadState.INVALID
        assert restarted.oldest(True).state == queue.ReadState.INVALID
        assert restarted.oldest(33).state == queue.ReadState.INVALID
        # Even outside the requested prefix, a corrupt receipt blocks *all* output.
        with sqlite3.connect(path) as db:
            valid = restarted.oldest(2).items[-1].wire
            bad = bytearray(valid)
            bad[-1] ^= 1
            db.execute('UPDATE kq05_receipt_outbox SET wire=? WHERE id=2',
                       (sqlite3.Binary(bytes(bad)),))
        assert restarted.oldest(1) == queue.ReadResult(queue.ReadState.REFUSED)
        assert restarted.enqueue(DEVICE, inc, EPOCH, (104,)).state == queue.PutState.REFUSED


def test_late_verified_conflict_fails_closed():
    with tempfile.TemporaryDirectory() as folder:
        store = ledger.VerifiedLocalLedger(Path(folder) / 'verified.db', keys)
        first = rows(1)[0]
        assert store.ingest(first).state == ledger.LedgerState.VERIFIED_STORED
        decoded = ledger.open_product_frame(first, keys)
        q = queue.BackendReceiptOutbox(batch.BackendBatchIssuer(store))
        assert q.enqueue(DEVICE, decoded.incarnation, EPOCH, (101,)).state == queue.PutState.STORED
        p = bytearray(decoded.plaintext)
        p[56:58] = (3800).to_bytes(2, 'big')
        alternative = reencrypt(public_fixture('kPeriodicFrameHex'), 9911, bytes(p), flags=0)
        assert store.ingest(alternative).state == ledger.LedgerState.CONTENT_CONFLICT
        # Earlier signed proof cannot bypass a newly discovered conflict.
        assert q.oldest(1).state == queue.ReadState.REFUSED
        assert q.enqueue(DEVICE, decoded.incarnation, EPOCH, (101,)).state == queue.PutState.NOT_DURABLE


def main():
    test_outbox_restart_bounds_and_selective_gap()
    test_late_verified_conflict_fails_closed()
    print('KQ05 Backend durable outbox: idempotent/restart/gap/conflict/whole-scan: PASS (HOST ONLY)')


if __name__ == '__main__':
    main()
