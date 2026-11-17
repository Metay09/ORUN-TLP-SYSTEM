#!/usr/bin/env python3
"""KQ05 host-only Backend -> Edge authenticated receipt transport + local commit."""
from __future__ import annotations

import copy
from pathlib import Path
import sqlite3
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "backend" / "candidates"))
sys.path.insert(0, str(ROOT / "edge" / "candidates"))
sys.path.insert(0, str(ROOT / "firmware" / "tests" / "sf5"))

import kq05_backend_batch_receipt as issuer_mod
import kq05_backend_receipt_outbox as backend_queue
import kq05_receipt_edge_transport as transport
import kq05_receipt_inbox as edge_queue
import product_secure_verified_ledger as ledger
from test_kq05_backend_batch_receipt import rows
from test_sf5h_backend_verified_ledger import DEVICE, EPOCH, keys, public_fixture, reencrypt

NONCE_A = "a1" * 16
NONCE_B = "b2" * 16
NONCE_C = "c3" * 16


def setup(folder: str):
    db = Path(folder) / "backend.sqlite"
    l = ledger.VerifiedLocalLedger(db, keys)
    records = rows(3)
    for frame in records:
        assert l.ingest(frame).state == ledger.LedgerState.VERIFIED_STORED
    inc = ledger.open_product_frame(records[0], keys).incarnation
    queue = backend_queue.BackendReceiptOutbox(issuer_mod.BackendBatchIssuer(l))
    for sequence in (101, 102, 103):
        assert queue.enqueue(DEVICE, inc, EPOCH, (sequence,)).state == backend_queue.PutState.STORED
    reg = transport.EdgeRegistration("edge-one", bytes(range(1, 33)), frozenset({DEVICE}))
    return l, queue, reg, inc


def test_authenticated_scoped_pages_and_restart():
    with tempfile.TemporaryDirectory() as d:
        _, outbox, reg, inc = setup(d)
        local = Path(d) / "edge.sqlite"
        edge = edge_queue.EdgeReceiptInbox(local, reg)
        r1 = transport.new_request(reg, 0, 2, nonce=NONCE_A)
        p1 = transport.get_page(outbox, {reg.edge_id: reg}, r1)
        assert p1.state == transport.PageState.READY
        assert p1.document["cursor"] == 2 and p1.document["more"] is True
        assert [i["id"] for i in p1.document["items"]] == [1, 2]
        assert transport.verify_response(reg, r1, p1.document)
        assert edge.accept_page(r1, p1.document) == edge_queue.StoreState.STORED
        assert edge.accept_page(r1, p1.document) == edge_queue.StoreState.DUPLICATE
        assert edge.scan().cursor == 2
        assert len(edge.scan().items) == 2

        reopened = edge_queue.EdgeReceiptInbox(local, reg)
        assert reopened.scan() == edge.scan()
        r2 = transport.new_request(reg, 2, 2, nonce=NONCE_B)
        p2 = transport.get_page(outbox, {reg.edge_id: reg}, r2)
        assert p2.state == transport.PageState.READY
        assert p2.document["cursor"] == 3 and p2.document["more"] is False
        assert reopened.accept_page(r2, p2.document) == edge_queue.StoreState.STORED
        assert len(reopened.scan().items) == 3
        assert reopened.accept_page(r1, p1.document) == edge_queue.StoreState.REFUSED
        # A complete and unchanged repoll never deletes, consumes or ACKs anything.
        r3 = transport.new_request(reg, 3, 2, nonce=NONCE_C)
        p3 = transport.get_page(outbox, {reg.edge_id: reg}, r3)
        assert p3.state == transport.PageState.EMPTY
        assert p3.document["cursor"] == 3 and not p3.document["items"]
        assert reopened.accept_page(r3, p3.document) == edge_queue.StoreState.EMPTY
        assert reopened.scan().cursor == 3
        assert transport.get_page(outbox, {reg.edge_id: reg}, r1).document == p1.document
        assert outbox.oldest(4).state == backend_queue.ReadState.READY

        # The authenticated receipt body is still only an *opaque* object at Edge.
        # No one here can falsely release Tracker flash or erase Backend records.
        assert reopened.scan().items[0].wire[4:12] == DEVICE
        assert reopened.scan().items[0].wire[12:20] == inc


def test_auth_scope_tamper_and_fail_closed():
    with tempfile.TemporaryDirectory() as d:
        _, outbox, reg, _ = setup(d)
        edge = edge_queue.EdgeReceiptInbox(Path(d) / "edge.sqlite", reg)
        r1 = transport.new_request(reg, 0, 2, nonce=NONCE_A)
        good = transport.get_page(outbox, {reg.edge_id: reg}, r1)
        forged = copy.deepcopy(good.document)
        first_wire = bytearray.fromhex(forged["items"][0]["wire"])
        first_wire[-1] ^= 1
        forged["items"][0]["wire"] = first_wire.hex()
        assert edge.accept_page(r1, forged) == edge_queue.StoreState.INVALID
        assert edge.scan().cursor == 0
        flipped = dict(r1, after=1)
        assert transport.get_page(outbox, {reg.edge_id: reg}, flipped).state == transport.PageState.REFUSED
        rogue = transport.EdgeRegistration("edge-one", b"z" * 32, frozenset({DEVICE}))
        wrong = transport.new_request(rogue, 0, 2, nonce=NONCE_A)
        assert transport.get_page(outbox, {reg.edge_id: reg}, wrong).state == transport.PageState.REFUSED
        other = transport.EdgeRegistration("edge-two", b"k" * 32, frozenset({b"OTHERDEV"}))
        r_other = transport.new_request(other, 0, 2, nonce=NONCE_A)
        empty = transport.get_page(outbox, {other.edge_id: other}, r_other)
        assert empty.state == transport.PageState.EMPTY
        assert empty.document["items"] == []
        with tempfile.TemporaryDirectory() as second:
            foreign_edge = edge_queue.EdgeReceiptInbox(Path(second) / "edge.sqlite", other)
            assert foreign_edge.accept_page(r1, good.document) == edge_queue.StoreState.INVALID
            assert foreign_edge.accept_page(r_other, empty.document) == edge_queue.StoreState.EMPTY
        assert transport.get_page(outbox, {}, r1).state == transport.PageState.REFUSED

        # Malformed request types are refused, including bool (an int subclass).
        for bad in ({**r1, "after": True}, {**r1, "limit": "2"},
                    {**r1, "nonce": "f" * 31}, {**r1, "mac": "0" * 64}):
            assert transport.get_page(outbox, {reg.edge_id: reg}, bad).state in (
                transport.PageState.INVALID, transport.PageState.REFUSED)
        assert not transport.verify_response(reg, {**r1, "limit": "two"}, good.document)

        # A damaged queued MAC blocks even an otherwise valid first page.
        with sqlite3.connect(outbox.issuer.ledger.path) as db:
            wire = bytearray(db.execute(
                "SELECT wire FROM kq05_receipt_outbox WHERE id=3").fetchone()[0])
            wire[-1] ^= 1
            db.execute("UPDATE kq05_receipt_outbox SET wire=? WHERE id=3",
                       (sqlite3.Binary(bytes(wire)),))
        assert transport.get_page(outbox, {reg.edge_id: reg}, r1).state == transport.PageState.REFUSED


def test_capacity_atomic_cursor_local_tamper():
    with tempfile.TemporaryDirectory() as d:
        _, outbox, reg, _ = setup(d)
        local = Path(d) / "edge.sqlite"
        edge = edge_queue.EdgeReceiptInbox(local, reg, max_items=1)
        big = transport.new_request(reg, 0, 2, nonce=NONCE_A)
        page = transport.get_page(outbox, {reg.edge_id: reg}, big)
        assert edge.accept_page(big, page.document) == edge_queue.StoreState.FULL
        assert edge.scan().cursor == 0 and edge.scan().items == ()
        small = transport.new_request(reg, 0, 1, nonce=NONCE_B)
        first = transport.get_page(outbox, {reg.edge_id: reg}, small)
        assert edge.accept_page(small, first.document) == edge_queue.StoreState.STORED
        assert edge.scan().cursor == 1
        with sqlite3.connect(local) as db:
            existing = bytearray(db.execute(
                "SELECT wire FROM kq05_edge_receipt_items WHERE id=1").fetchone()[0])
            existing[-1] ^= 1
            db.execute("UPDATE kq05_edge_receipt_items SET wire=? WHERE id=1",
                       (sqlite3.Binary(bytes(existing)),))
        assert edge.scan().state == edge_queue.StoreState.REFUSED
        assert edge.accept_page(small, first.document) == edge_queue.StoreState.REFUSED
        # Cross-installation DB reuse cannot silently assign old receipts.
        other = transport.EdgeRegistration("edge-different", b"j" * 32,
                                           frozenset({DEVICE}))
        try:
            edge_queue.EdgeReceiptInbox(local, other)
        except ValueError:
            pass
        else:
            raise AssertionError("foreign Edge identity must be denied")


def test_late_conflict_invalidates_authenticated_page():
    with tempfile.TemporaryDirectory() as d:
        store, outbox, reg, _ = setup(d)
        req = transport.new_request(reg, 0, 2, nonce=NONCE_A)
        assert transport.get_page(outbox, {reg.edge_id: reg}, req).state == transport.PageState.READY
        prior = rows(1)[0]
        p = bytearray(ledger.open_product_frame(prior, keys).plaintext)
        p[56:58] = (3855).to_bytes(2, "big")
        conflict = reencrypt(public_fixture("kPeriodicFrameHex"), 9983, bytes(p), flags=0)
        assert store.ingest(conflict).state == ledger.LedgerState.CONTENT_CONFLICT
        assert transport.get_page(outbox, {reg.edge_id: reg}, req).state == transport.PageState.REFUSED


def main():
    test_authenticated_scoped_pages_and_restart()
    test_auth_scope_tamper_and_fail_closed()
    test_capacity_atomic_cursor_local_tamper()
    test_late_conflict_invalidates_authenticated_page()
    print("KQ05 Backend->Edge signed scoped pages / durable inbox / retry: PASS (HOST ONLY)")


if __name__ == "__main__":
    main()
