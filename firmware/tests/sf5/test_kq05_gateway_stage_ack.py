#!/usr/bin/env python3
"""KQ05 Gateway persisted-stage ACK: never a Tracker/Backend release (host)."""
from __future__ import annotations

from pathlib import Path
import sqlite3
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[3]
for rel in ("backend/candidates", "edge/candidates", "gateway/candidates",
            "firmware/tests/sf5"):
    sys.path.insert(0, str(ROOT / rel))

import kq05_receipt_edge_transport as transport
import kq05_receipt_inbox as incoming
import kq05_edge_gateway_offer as offer
import kq05_gateway_receipt_stage as staging
import kq05_gateway_stage_ack as ack
from test_kq05_receipt_edge_transport import setup
from test_sf5h_backend_verified_ledger import DEVICE


def setup_path(folder, *, capacity=2):
    _, backend, registration, _ = setup(folder)
    edge = incoming.EdgeReceiptInbox(Path(folder) / "edge.sqlite", registration)
    req = transport.new_request(registration, 0, 2, nonce="be" * 16)
    page = transport.get_page(backend, {registration.edge_id: registration}, req)
    assert edge.accept_page(req, page.document) == incoming.StoreState.STORED
    permit = offer.GatewayLinkPermit(registration.edge_id,
                                     b"GATEWAY1", b"k" * 32,
                                     frozenset({DEVICE}))
    gateway = staging.GatewayReceiptStage(
        Path(folder) / "gw.sqlite", permit, capacity=capacity)
    first = offer.from_edge_inbox(edge, permit, 1)
    second = offer.from_edge_inbox(edge, permit, 2)
    assert first.state == second.state == offer.OfferState.READY
    return backend, edge, permit, gateway, first.data, second.data


def test_commit_ack_retry_and_restart():
    with tempfile.TemporaryDirectory() as folder:
        backend, edge, permit, gateway, first, second = setup_path(folder)
        result = ack.gateway_accept_and_reply(gateway, first)
        assert result.state == ack.ExchangeState.CONFIRMED
        proof = ack.edge_verify_stage_result(first, result.response, permit)
        assert proof == ack.EdgeStageConfirmation(1, False)
        assert ack.edge_verify_stage_result(second, result.response, permit) is None
        assert edge.scan().items[0].row_id == 1
        assert len(edge.scan().items) == 2
        assert backend.oldest(4).state.name == "READY"
        for pos in range(len(result.response)):
            bad = bytearray(result.response)
            bad[pos] ^= 1
            assert ack.edge_verify_stage_result(first, bytes(bad), permit) is None
        assert ack.edge_verify_stage_result(first, result.response[:-1], permit) is None
        restarted = staging.GatewayReceiptStage(
            Path(folder) / "gw.sqlite", permit, capacity=2)
        again = ack.gateway_accept_and_reply(restarted, first)
        assert again.state == ack.ExchangeState.CONFIRMED
        assert ack.edge_verify_stage_result(first, again.response, permit) == (
            ack.EdgeStageConfirmation(1, True))
        second_answer = ack.gateway_accept_and_reply(restarted, second)
        assert second_answer.state == ack.ExchangeState.CONFIRMED
        assert ack.edge_verify_stage_result(second, second_answer.response, permit) == (
            ack.EdgeStageConfirmation(2, False))
        assert len(restarted.scan().items) == 2
        # Never equate any STAGED proof with tracker release / DB delivery.
        assert backend.oldest(4).state.name == "READY"
        assert len(edge.scan().items) == 2


def test_failure_scope_full_and_corruption_never_ack():
    with tempfile.TemporaryDirectory() as folder:
        _, edge, permit, gateway, first, second = setup_path(folder, capacity=1)
        correct = ack.gateway_accept_and_reply(gateway, first)
        assert correct.state == ack.ExchangeState.CONFIRMED
        assert ack.gateway_accept_and_reply(gateway, second) == (
            ack.ExchangeResult(ack.ExchangeState.RETRY))
        assert len(gateway.scan().items) == 1
        other = offer.GatewayLinkPermit(permit.edge_id, b"GATEWAY2",
                                        permit.key, permit.allowed_devices)
        assert ack.edge_verify_stage_result(first, correct.response, other) is None
        foreign = offer.GatewayLinkPermit("other-edge", permit.gateway_id,
                                          permit.key, permit.allowed_devices)
        assert ack.edge_verify_stage_result(first, correct.response, foreign) is None
        invalid = bytearray(first)
        invalid[-1] ^= 1
        assert ack.gateway_accept_and_reply(gateway, bytes(invalid)) == (
            ack.ExchangeResult(ack.ExchangeState.REFUSED))
        assert ack.gateway_accept_and_reply(gateway, b"") == (
            ack.ExchangeResult(ack.ExchangeState.REFUSED))

        with sqlite3.connect(gateway.path) as db:
            row = bytearray(db.execute(
                "SELECT wire FROM kq05_gateway_receipts WHERE id=1").fetchone()[0])
            row[-1] ^= 1
            db.execute("UPDATE kq05_gateway_receipts SET wire=? WHERE id=1",
                       (sqlite3.Binary(bytes(row)),))
        assert gateway.scan().state == staging.StageState.REFUSED
        assert ack.gateway_accept_and_reply(gateway, first).state == ack.ExchangeState.REFUSED
        assert len(edge.scan().items) == 2


def main():
    test_commit_ack_retry_and_restart()
    test_failure_scope_full_and_corruption_never_ack()
    print("KQ05 Gateway durable STAGED result: MAC/binding/reboot/full/tamper PASS (HOST ONLY)")


if __name__ == "__main__":
    main()
