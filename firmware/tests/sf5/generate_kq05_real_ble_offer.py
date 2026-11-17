#!/usr/bin/env python3
"""KQ05 LAB: write the *actual* Backend -> Edge -> BLE offer bytes.

Output is a local test fixture only, never a live Gateway transmission.
The OGK1 key in this fixture is publicly known and MUST NOT be deployed.
"""
from pathlib import Path
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[3]
for rel in ("backend/candidates", "edge/candidates", "firmware/tests/sf5"):
    sys.path.insert(0, str(ROOT / rel))

import kq05_receipt_edge_transport as backend_transport
from kq05_receipt_inbox import EdgeReceiptInbox, StoreState
from kq05_edge_gateway_offer import GatewayLinkPermit, OfferState, from_edge_inbox
from kq05_gateway_link_derive import derive_gateway_offer_key
from test_kq05_receipt_edge_transport import setup
from test_sf5h_backend_verified_ledger import DEVICE


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: generate_kq05_real_ble_offer.py <local output file>")
    dest = Path(sys.argv[1])
    if dest.exists() or dest.is_symlink():
        raise SystemExit("refusing to overwrite a fixture")
    with tempfile.TemporaryDirectory() as folder:
        _, backend_outbox, registration, _ = setup(folder)
        inbox = EdgeReceiptInbox(Path(folder) / "edge-inbox.sqlite", registration)
        request = backend_transport.new_request(
            registration, 0, 1, nonce="a7" * 16)
        page = backend_transport.get_page(
            backend_outbox, {registration.edge_id: registration}, request)
        assert page.state == backend_transport.PageState.READY
        assert inbox.accept_page(request, page.document) == StoreState.STORED

        # Dummy public test enrollment, never production credentials.
        key = derive_gateway_offer_key(
            bytes(range(0x40, 0x60)), b"GATEWAY1", "edge-one")
        permit = GatewayLinkPermit(
            registration.edge_id, b"GATEWAY1", key, frozenset({DEVICE}))
        offer = from_edge_inbox(inbox, permit, 1)
        assert offer.state == OfferState.READY
        logical = b"\x91" + offer.data
        assert len(logical) == 96 and logical[0] == 0x91
        assert logical[1 + 19 + 4:1 + 19 + 12] == DEVICE
        assert logical[1 + 19 + 20:1 + 19 + 24] == (0x01020304).to_bytes(4, "big")
        dest.write_bytes(logical)
        assert dest.stat().st_size == 96
        assert len(inbox.scan().items) == 1
        assert backend_outbox.oldest(4).state.name == "READY"


if __name__ == "__main__":
    main()
