#!/usr/bin/env python3
"""KQ05 real Backend->Edge->Gateway logical BLE STAGED (HOST ONLY).

Uses the real verified Backend SQLite receipt, durable Edge inbox, exact
0x91 message bytes, independently committed/read-back Gateway SQLite, exact
0x92 authenticated STAGED reply and Edge-side verification. No RAK/RF.
"""
from __future__ import annotations

from pathlib import Path
import sqlite3
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[3]
for rel in ("backend/candidates", "edge/candidates", "gateway/candidates",
            "firmware/tests/sf5"):
    sys.path.insert(0, str(ROOT / rel))

import kq05_edge_gateway_offer as offer
import kq05_gateway_receipt_stage as staging
import kq05_gateway_stage_ack as ack
import kq05_gateway_ble_durable_stage_bridge as bridge
from test_kq05_edge_gateway_stage import deliver_backend_page


def wrapped(edge, permit, source_row: int) -> tuple[bytes, bytes]:
    value = offer.from_edge_inbox(edge, permit, source_row)
    assert value.state == offer.OfferState.READY
    logical = bytes([0x91]) + value.data
    assert 96 <= len(logical) <= 124
    return logical, value.data


def test_full_cross_owner_roundtrip_and_restart() -> None:
    with tempfile.TemporaryDirectory() as folder:
        backend, edge, permit, _inc = deliver_backend_page(folder)
        path = Path(folder) / "gateway-stage.sqlite"
        gateway = staging.GatewayReceiptStage(path, permit, capacity=1)
        receiver = bridge.GatewayBleDurableStageBridge(gateway)
        logical, inner = wrapped(edge, permit, 1)

        # Simulated policy gates are independent; no BLE byte may authorize
        # either a Gateway service or a trusted Edge installation.
        for gw, installation in ((False, False), (True, False),
                                 (False, True)):
            result = receiver.receive(
                logical,
                authorized_gateway_service=gw,
                verified_edge_installation=installation)
            assert result == bridge.BridgeResult(bridge.BridgeState.REFUSED)
            assert not gateway.scan().items

        first = receiver.receive(
            logical, authorized_gateway_service=True,
            verified_edge_installation=True)
        assert first.state == bridge.BridgeState.STAGED
        assert len(first.response) == 84 and first.response[0] == 0x92
        confirmation = ack.edge_verify_stage_result(
            inner, first.response[1:], permit)
        assert confirmation == ack.EdgeStageConfirmation(1, False)

        # Reboot/reopen is not a new delivery credit. The exact same record
        # survives, and receives an authenticated ALREADY response.
        reopened = staging.GatewayReceiptStage(path, permit, capacity=1)
        again = bridge.GatewayBleDurableStageBridge(reopened).receive(
            logical, authorized_gateway_service=True,
            verified_edge_installation=True)
        assert again.state == bridge.BridgeState.STAGED
        assert ack.edge_verify_stage_result(
            inner, again.response[1:], permit) == (
                ack.EdgeStageConfirmation(1, True))
        assert len(reopened.scan().items) == 1

        # Full Gateway never sends STAGED for the second offer.
        second, second_inner = wrapped(edge, permit, 2)
        full = bridge.GatewayBleDurableStageBridge(reopened).receive(
            second, authorized_gateway_service=True,
            verified_edge_installation=True)
        assert full == bridge.BridgeResult(bridge.BridgeState.RETRY)
        assert ack.edge_verify_stage_result(
            second_inner, first.response[1:], permit) is None

        # STAGED is only Gateway-local storage evidence: neither Backend nor
        # Edge may delete their pending receipt, and no Tracker marker exists.
        assert backend.oldest(4).state.name == "READY"
        assert len(edge.scan().items) == 2
        assert [item.source_row for item in reopened.scan().items] == [1]


def test_wrong_scope_corruption_and_no_false_staged() -> None:
    with tempfile.TemporaryDirectory() as folder:
        _backend, edge, permit, _inc = deliver_backend_page(folder)
        path = Path(folder) / "gateway-stage.sqlite"
        gateway = staging.GatewayReceiptStage(path, permit, capacity=2)
        receiver = bridge.GatewayBleDurableStageBridge(gateway)
        logical, _inner = wrapped(edge, permit, 1)

        for invalid in (b"", b"\x91", bytes([0x92]) + logical[1:],
                        logical[:-1], logical + b"\x00",
                        bytearray(logical),  # mutable input must fail closed
                        { "data": logical }, None, True):
            result = receiver.receive(
                invalid, authorized_gateway_service=True,
                verified_edge_installation=True)
            assert result == bridge.BridgeResult(bridge.BridgeState.REFUSED)

        damaged = bytearray(logical)
        damaged[-1] ^= 1
        assert receiver.receive(
            bytes(damaged), authorized_gateway_service=True,
            verified_edge_installation=True).state == bridge.BridgeState.REFUSED
        damaged = bytearray(logical)
        damaged[1 + 19 + 20] ^= 1  # epoch / Edge MAC mismatch
        assert receiver.receive(
            bytes(damaged), authorized_gateway_service=True,
            verified_edge_installation=True).state == bridge.BridgeState.REFUSED
        assert gateway.scan().items == ()

        # A different, correctly configured Gateway cannot stage an offer
        # addressed to this Gateway, even with the same Edge installation.
        foreign_permit = offer.GatewayLinkPermit(
            permit.edge_id, b"GATEWAY2", permit.key, permit.allowed_devices)
        foreign_store = staging.GatewayReceiptStage(
            Path(folder) / "foreign-gateway.sqlite", foreign_permit,
            capacity=2)
        foreign = bridge.GatewayBleDurableStageBridge(foreign_store)
        assert foreign.receive(
            logical, authorized_gateway_service=True,
            verified_edge_installation=True) == (
                bridge.BridgeResult(bridge.BridgeState.REFUSED))
        assert foreign_store.scan().items == ()

        original = receiver.receive(
            logical, authorized_gateway_service=True,
            verified_edge_installation=True)
        assert original.state == bridge.BridgeState.STAGED

        # A corrupt persisted receipt forbids even an idempotent ALREADY.
        with sqlite3.connect(path) as db:
            row = db.execute(
                "SELECT tag FROM kq05_gateway_receipts WHERE id=1"
            ).fetchone()
            assert row is not None
            tag = bytearray(row[0])
            tag[-1] ^= 1
            db.execute(
                "UPDATE kq05_gateway_receipts SET tag=? WHERE id=1",
                (sqlite3.Binary(bytes(tag)),))
        assert receiver.receive(
            logical, authorized_gateway_service=True,
            verified_edge_installation=True).state == bridge.BridgeState.REFUSED
        assert receiver.receive(
            logical, authorized_gateway_service=True,
            verified_edge_installation=True).response == b""


def main() -> None:
    test_full_cross_owner_roundtrip_and_restart()
    test_wrong_scope_corruption_and_no_false_staged()
    print(
        "KQ05 REAL Backend->Edge 0x91 Gateway durable STAGED 0x92 "
        "restart/full/tamper/auth: PASS (HOST ONLY)"
    )


if __name__ == "__main__":
    main()
