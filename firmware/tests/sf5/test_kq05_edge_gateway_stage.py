#!/usr/bin/env python3
"""KQ05: Backend->Edge->Gateway durable staging (HOST only, no RF claims)."""
from __future__ import annotations

from pathlib import Path
import sqlite3
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "backend" / "candidates"))
sys.path.insert(0, str(ROOT / "edge" / "candidates"))
sys.path.insert(0, str(ROOT / "gateway" / "candidates"))
sys.path.insert(0, str(ROOT / "firmware" / "tests" / "sf5"))

import kq05_receipt_edge_transport as transport
import kq05_receipt_inbox as incoming
import kq05_edge_gateway_offer as offer
import kq05_gateway_receipt_stage as staging
from test_kq05_receipt_edge_transport import setup
from test_sf5h_backend_verified_ledger import CRED, DEVICE, EPOCH
from kq05_backend_batch_receipt import verify_for_tracker


def deliver_backend_page(folder: str):
    _, outbox, reg, inc = setup(folder)
    edge = incoming.EdgeReceiptInbox(Path(folder) / "edge_inbox.sqlite", reg)
    req = transport.new_request(reg, 0, 2, nonce="ac" * 16)
    page = transport.get_page(outbox, {reg.edge_id: reg}, req)
    assert page.state == transport.PageState.READY
    assert edge.accept_page(req, page.document) == incoming.StoreState.STORED
    permit = offer.GatewayLinkPermit(
        reg.edge_id, b"GATEWAY1", b"K" * 32, frozenset({DEVICE}))
    return outbox, edge, permit, inc


def test_gateway_staging_exact_and_restart():
    with tempfile.TemporaryDirectory() as d:
        outbox, edge, permit, inc = deliver_backend_page(d)
        gw_db = Path(d) / "gateway_stage.sqlite"
        gw = staging.GatewayReceiptStage(gw_db, permit, capacity=2)
        first = offer.from_edge_inbox(edge, permit, 1)
        assert first.state == offer.OfferState.READY
        assert 95 <= len(first.data) <= 123  # <= current BLE logical 160 bytes
        checked = offer.verify_offer(first.data, permit)
        assert checked and checked.source_row == 1
        assert verify_for_tracker(checked.wire, CRED, DEVICE, inc, EPOCH) == (101,)
        assert gw.accept(first.data) == staging.StageResult(staging.StageState.STAGED, 1)
        assert gw.accept(first.data) == staging.StageResult(staging.StageState.DUPLICATE, 1)
        second = offer.from_edge_inbox(edge, permit, 2)
        assert second.state == offer.OfferState.READY
        assert gw.accept(second.data) == staging.StageResult(staging.StageState.STAGED, 2)
        initial = gw.scan(limit=1)
        assert initial.state == staging.StageState.STAGED and initial.more is True
        assert [x.source_row for x in gw.scan(limit=8).items] == [1, 2]
        assert [x.source_row for x in gw.scan(limit=8, after_id=1).items] == [2]
        assert gw.scan(limit=0).state == staging.StageState.REFUSED
        assert gw.scan(limit=True).state == staging.StageState.REFUSED

        restarted = staging.GatewayReceiptStage(gw_db, permit, capacity=2)
        assert restarted.scan() == gw.scan()
        assert restarted.accept(first.data).state == staging.StageState.DUPLICATE
        assert offer.from_edge_inbox(edge, permit, 3).state == offer.OfferState.NOT_FOUND
        assert outbox.oldest(8).state.name == "READY"  # never erased

        # Edge receives the third Backend receipt, but Gateway is full.
        reg = edge.registration
        req = transport.new_request(reg, 2, 2, nonce="ad" * 16)
        page = transport.get_page(outbox, {reg.edge_id: reg}, req)
        assert edge.accept_page(req, page.document) == incoming.StoreState.STORED
        third = offer.from_edge_inbox(edge, permit, 3)
        assert third.state == offer.OfferState.READY
        assert restarted.accept(third.data).state == staging.StageState.FULL
        assert len(restarted.scan().items) == 2
        assert len(edge.scan().items) == 3  # full Gateway never deletes Edge records
        assert restarted.accept(second.data).state == staging.StageState.DUPLICATE
        assert outbox.oldest(8).state.name == "READY"  # Backend unchanged


def test_auth_scope_tamper_conflict_and_full_scan():
    with tempfile.TemporaryDirectory() as d:
        _, edge, permit, _ = deliver_backend_page(d)
        gw_db = Path(d) / "gateway_stage.sqlite"
        gw = staging.GatewayReceiptStage(gw_db, permit, capacity=2)
        first = offer.from_edge_inbox(edge, permit, 1)
        second = offer.from_edge_inbox(edge, permit, 2)
        assert gw.accept(first.data).state == staging.StageState.STAGED
        assert gw.accept(second.data).state == staging.StageState.STAGED

        for i in range(len(first.data)):
            tampered = bytearray(first.data)
            tampered[i] ^= 1
            assert gw.accept(bytes(tampered)).state == staging.StageState.REFUSED

        other_gateway = offer.GatewayLinkPermit(
            permit.edge_id, b"GATEWAY2", permit.key, frozenset({DEVICE}))
        wrong_key = offer.GatewayLinkPermit(
            permit.edge_id, permit.gateway_id, b"J" * 32, frozenset({DEVICE}))
        wrong_device = offer.GatewayLinkPermit(
            permit.edge_id, permit.gateway_id, permit.key, frozenset({b"OTHERDEV"}))
        assert offer.verify_offer(first.data, other_gateway) is None
        assert offer.verify_offer(first.data, wrong_key) is None
        assert offer.verify_offer(first.data, wrong_device) is None
        for foreign in (other_gateway, wrong_key):
            try:
                staging.GatewayReceiptStage(gw_db, foreign)
            except ValueError:
                pass
            else:
                raise AssertionError("Gateway stage DB must be installation scoped")

        # Same authenticated Edge row ID with different receipt is a conflict.
        changed = bytearray(second.data[:-32])
        changed[10:18] = (1).to_bytes(8, "big")
        conflicting_offer = bytes(changed) + offer._mac(bytes(changed), permit)
        assert offer.verify_offer(conflicting_offer, permit) is not None
        assert gw.accept(conflicting_offer).state == staging.StageState.REFUSED

        # Corruption of the second row prevents even the first-page scan.
        with sqlite3.connect(gw_db) as db:
            bad = bytearray(db.execute(
                "SELECT wire FROM kq05_gateway_receipts WHERE id=2").fetchone()[0])
            bad[-1] ^= 1
            db.execute("UPDATE kq05_gateway_receipts SET wire=? WHERE id=2",
                       (sqlite3.Binary(bytes(bad)),))
        assert gw.scan(limit=1).state == staging.StageState.REFUSED
        assert gw.accept(first.data).state == staging.StageState.REFUSED


def test_edge_corruption_blocks_offer_and_cross_account_scope():
    with tempfile.TemporaryDirectory() as d:
        _, edge, permit, _ = deliver_backend_page(d)
        wrong = offer.GatewayLinkPermit(
            "edge-other", permit.gateway_id, permit.key, frozenset({DEVICE}))
        assert offer.from_edge_inbox(edge, wrong, 1).state == offer.OfferState.INVALID
        disallowed = offer.GatewayLinkPermit(
            permit.edge_id, permit.gateway_id, permit.key,
            frozenset({b"OTHERDEV"}))
        assert offer.from_edge_inbox(edge, disallowed, 1).state == offer.OfferState.INVALID
        assert offer.from_edge_inbox(edge, permit, True).state == offer.OfferState.INVALID
        with sqlite3.connect(edge.path) as db:
            bad = bytearray(db.execute(
                "SELECT wire FROM kq05_edge_receipt_items WHERE id=2").fetchone()[0])
            bad[-1] ^= 1
            db.execute("UPDATE kq05_edge_receipt_items SET wire=? WHERE id=2",
                       (sqlite3.Binary(bytes(bad)),))
        assert edge.scan().state == incoming.StoreState.REFUSED
        assert offer.from_edge_inbox(edge, permit, 1).state == offer.OfferState.REFUSED


def main():
    test_gateway_staging_exact_and_restart()
    test_auth_scope_tamper_conflict_and_full_scan()
    test_edge_corruption_blocks_offer_and_cross_account_scope()
    print("KQ05 Edge->Gateway authenticated durable staging/reboot/full/tamper: PASS (HOST ONLY)")


if __name__ == "__main__":
    main()
