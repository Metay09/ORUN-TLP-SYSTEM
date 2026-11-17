"""KQ05 LAB only: build an authenticated Edge->Gateway receipt offer.

No USB/BLE transmission, Gateway RF operation, single ACK replacement,
Tracker release or Backend deletion. A Gateway stages opaque Backend receipt
bytes; it cannot check the Backend->Tracker HMAC without K_root.
The Edge/Gateway transport key MUST be distinct from Backend/Edge auth keys.
"""
from __future__ import annotations

from dataclasses import dataclass
from enum import Enum, auto
import hmac
from hashlib import sha256
import re
import struct

from kq05_receipt_inbox import EdgeReceiptInbox, StoreState

# Experimental only; not assigned in the production edge_link Type registry.
_VERSION = 1
_TYPE = 0x91
_PREFIX = struct.Struct(">BB8sQB")  # version, type, gw, source Edge row ID, size
_DOMAIN = b"ORUN-KQ05-EDGE-GATEWAY-OFFER-v1\x00"
_TAG_LEN = 32
_MAX_FRAME = 160  # current edge_link::kMaxMessage
_MAX_ID = 9223372036854775807
_EDGE_ID = re.compile(r"[A-Za-z0-9_-]{1,64}\Z", re.ASCII)


@dataclass(frozen=True)
class GatewayLinkPermit:
    edge_id: str
    gateway_id: bytes
    key: bytes
    allowed_devices: frozenset[bytes]

    def __post_init__(self):
        if (type(self.edge_id) is not str or not _EDGE_ID.fullmatch(self.edge_id) or
                type(self.gateway_id) is not bytes or len(self.gateway_id) != 8 or
                not any(self.gateway_id) or type(self.key) is not bytes or
                len(self.key) != 32 or not any(self.key) or
                type(self.allowed_devices) is not frozenset or
                not 1 <= len(self.allowed_devices) <= 128 or
                any(type(d) is not bytes or len(d) != 8 or not any(d)
                    for d in self.allowed_devices)):
            raise ValueError("invalid trusted Gateway link permit")


class OfferState(Enum):
    READY = auto()
    NOT_FOUND = auto()
    INVALID = auto()
    REFUSED = auto()
    UNAVAILABLE = auto()


@dataclass(frozen=True)
class OfferResult:
    state: OfferState
    data: bytes = b""


@dataclass(frozen=True)
class VerifiedOffer:
    source_row: int
    wire: bytes


def receipt_shape(wire: bytes, permit: GatewayLinkPermit) -> bool:
    """Gateway can check length/target/IDs, NOT the embedded Backend HMAC."""
    if (type(wire) is not bytes or not 44 <= len(wire) <= 72 or
            wire[:3] != b"\x02\x09\x00" or not 1 <= wire[3] <= 8 or
            len(wire) != 40 + 4 * wire[3] or
            wire[4:12] not in permit.allowed_devices or
            not any(wire[12:20]) or wire[20:24] == b"\xff" * 4):
        return False
    seqs = struct.unpack(">" + "I" * wire[3],
                         wire[24:24 + 4 * wire[3]])
    return all(v > 0 for v in seqs) and all(a < b for a, b in zip(seqs, seqs[1:]))


def _mac(data: bytes, permit: GatewayLinkPermit) -> bytes:
    return hmac.new(
        permit.key, _DOMAIN + permit.edge_id.encode("ascii") + b"\x00" + data,
        sha256).digest()


def from_edge_inbox(inbox: EdgeReceiptInbox, permit: GatewayLinkPermit,
                    source_row: int) -> OfferResult:
    """Only copies a previously MAC-verified, durable Edge inbox record."""
    if (not isinstance(inbox, EdgeReceiptInbox) or
            not isinstance(permit, GatewayLinkPermit) or
            inbox.registration.edge_id != permit.edge_id or
            type(source_row) is not int or not 1 <= source_row <= _MAX_ID or
            not permit.allowed_devices.issubset(inbox.registration.allowed_devices)):
        return OfferResult(OfferState.INVALID)
    snapshot = inbox.scan()
    if snapshot.state == StoreState.REFUSED:
        return OfferResult(OfferState.REFUSED)
    if snapshot.state == StoreState.UNAVAILABLE:
        return OfferResult(OfferState.UNAVAILABLE)
    item = next((x for x in snapshot.items if x.row_id == source_row), None)
    if item is None:
        return OfferResult(OfferState.NOT_FOUND)
    if not receipt_shape(item.wire, permit):
        return OfferResult(OfferState.REFUSED)
    unsigned = _PREFIX.pack(
        _VERSION, _TYPE, permit.gateway_id, source_row, len(item.wire)
    ) + item.wire
    frame = unsigned + _mac(unsigned, permit)
    if len(frame) > _MAX_FRAME:
        return OfferResult(OfferState.INVALID)
    return OfferResult(OfferState.READY, frame)


def verify_offer(data: object, permit: GatewayLinkPermit) -> VerifiedOffer | None:
    """Only trusted Gateway link enrollment supplies permit, never wire input."""
    if (not isinstance(permit, GatewayLinkPermit) or type(data) is not bytes or
            len(data) < _PREFIX.size + 44 + _TAG_LEN or
            len(data) > _MAX_FRAME):
        return None
    version, kind, gateway, source_row, size = _PREFIX.unpack_from(data)
    if ((version, kind, gateway) != (_VERSION, _TYPE, permit.gateway_id) or
            not 1 <= source_row <= _MAX_ID or not 44 <= size <= 72 or
            len(data) != _PREFIX.size + size + _TAG_LEN):
        return None
    unsigned = data[:-_TAG_LEN]
    if not hmac.compare_digest(_mac(unsigned, permit), data[-_TAG_LEN:]):
        return None
    wire = data[_PREFIX.size:-_TAG_LEN]
    if not receipt_shape(wire, permit):
        return None
    return VerifiedOffer(source_row, wire)
