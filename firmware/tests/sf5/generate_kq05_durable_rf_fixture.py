#!/usr/bin/env python3
"""KQ05 PC fixture: true Backend AEAD -> Edge durable inbox -> Gateway SQLite
durable stage -> authenticated 0x92 STAGED, exported for native radio policy.

No RAK device, physical LoRa, field keys, production DB or service writes.
"""
from __future__ import annotations

from pathlib import Path
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[3]
for path in ("backend/candidates", "edge/candidates", "gateway/candidates",
             "firmware/tests/sf5"):
    sys.path.insert(0, str(ROOT / path))

import kq05_receipt_edge_transport as transport
from kq05_receipt_inbox import EdgeReceiptInbox, StoreState
from kq05_edge_gateway_offer import (
    GatewayLinkPermit, OfferState, from_edge_inbox,
)
from kq05_gateway_link_derive import derive_gateway_offer_key
from kq05_gateway_receipt_stage import GatewayReceiptStage
from kq05_gateway_ble_durable_stage_bridge import (
    BridgeState, GatewayBleDurableStageBridge,
)
from kq05_gateway_stage_ack import (
    EdgeStageConfirmation, edge_verify_stage_result,
)
from test_kq05_receipt_edge_transport import setup
from test_sf5h_backend_verified_ledger import DEVICE


def main() -> None:
    if len(sys.argv) != 3:
        raise SystemExit("usage: generate_kq05_durable_rf_fixture.py OFFER STAGED")
    offer_path, stage_path = (Path(x) for x in sys.argv[1:])
    if (offer_path == stage_path or
            any(p.exists() or p.is_symlink() for p in (offer_path, stage_path))):
        raise SystemExit("refusing to overwrite test output")
    with tempfile.TemporaryDirectory() as folder:
        _ledger, backend, registration, _inc = setup(folder)
        edge = EdgeReceiptInbox(Path(folder) / "edge.sqlite", registration)
        request = transport.new_request(
            registration, 0, 1, nonce="a7" * 16)
        page = transport.get_page(
            backend, {registration.edge_id: registration}, request)
        assert page.state == transport.PageState.READY
        assert edge.accept_page(request, page.document) == StoreState.STORED

        # PUBLIC test vector, NOT an enrolled physical Gateway installation.
        key = derive_gateway_offer_key(
            bytes(range(0x40, 0x60)), b"GATEWAY1", "edge-one")
        permit = GatewayLinkPermit(
            registration.edge_id, b"GATEWAY1", key, frozenset({DEVICE}))
        offer = from_edge_inbox(edge, permit, 1)
        assert offer.state == OfferState.READY
        logical_offer = bytes([0x91]) + offer.data
        assert len(logical_offer) == 96

        location = Path(folder) / "gateway.sqlite"
        stored = GatewayBleDurableStageBridge(
            GatewayReceiptStage(location, permit, capacity=2)).receive(
                logical_offer, authorized_gateway_service=True,
                verified_edge_installation=True)
        assert stored.state == BridgeState.STAGED
        assert len(stored.response) == 84 and stored.response[0] == 0x92
        assert edge_verify_stage_result(
            offer.data, stored.response[1:], permit) == (
                EdgeStageConfirmation(1, False))

        # Recovered Gateway must retain EXACT bytes. Neither Edge nor Backend
        # may discard its copy because the Gateway sent a STAGED result.
        recovered = GatewayBleDurableStageBridge(
            GatewayReceiptStage(location, permit, capacity=2)).receive(
                logical_offer, authorized_gateway_service=True,
                verified_edge_installation=True)
        assert recovered.state == BridgeState.STAGED
        assert edge_verify_stage_result(
            offer.data, recovered.response[1:], permit) == (
                EdgeStageConfirmation(1, True))
        assert backend.oldest(3).state.name == "READY"
        assert len(edge.scan().items) == 1

        offer_path.write_bytes(logical_offer)
        stage_path.write_bytes(stored.response)
        assert offer_path.stat().st_size == 96
        assert stage_path.stat().st_size == 84


if __name__ == "__main__":
    main()
