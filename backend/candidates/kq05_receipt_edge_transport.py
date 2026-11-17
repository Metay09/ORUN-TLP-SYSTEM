"""KQ05 LAB: authenticated, device-scoped Backend -> Edge receipt PAGE protocol.

No public HTTP endpoint, live service mutation, per-device root key export, RF,
or Tracker release is performed here. Caller must provision a unique 32-byte
transport key per enrolled Edge; use a protected HTTPS channel for privacy.
This protocol authenticates *read-only retrieval*. Neither pulling a page nor
storing it at Edge means the Gateway or Tracker received a receipt.
"""
from __future__ import annotations

from dataclasses import dataclass
from enum import Enum, auto
import hmac
from hashlib import sha256
import json
import re
import secrets
from typing import Mapping

from kq05_backend_receipt_outbox import BackendReceiptOutbox, ReadState

_EDGE = re.compile(r"[A-Za-z0-9_-]{1,64}\Z", re.ASCII)
_NONCE = re.compile(r"[0-9a-f]{32}\Z", re.ASCII)
_MAC = re.compile(r"[0-9a-f]{64}\Z", re.ASCII)
_REQ_DOMAIN = b"ORUN-KQ05-EDGE-PULL-REQ-V1\x00"
_RESP_DOMAIN = b"ORUN-KQ05-EDGE-PULL-RESP-V1\x00"
_CURSOR_MAX = 9223372036854775807


@dataclass(frozen=True)
class EdgeRegistration:
    edge_id: str
    transport_key: bytes
    allowed_devices: frozenset[bytes]

    def __post_init__(self):
        if (type(self.edge_id) is not str or not _EDGE.fullmatch(self.edge_id) or
                type(self.transport_key) is not bytes or len(self.transport_key) != 32 or
                not any(self.transport_key) or type(self.allowed_devices) is not frozenset or
                not 1 <= len(self.allowed_devices) <= 128 or
                any(type(x) is not bytes or len(x) != 8 or not any(x)
                    for x in self.allowed_devices)):
            raise ValueError("invalid trusted Edge registration")


class PageState(Enum):
    READY = auto()
    EMPTY = auto()
    INVALID = auto()
    REFUSED = auto()
    UNAVAILABLE = auto()


@dataclass(frozen=True)
class PageResult:
    state: PageState
    document: dict | None = None


def _tag(key: bytes, domain: bytes, value: dict) -> str:
    canonical = json.dumps(value, sort_keys=True, separators=(",", ":"),
                           ensure_ascii=True, allow_nan=False).encode("ascii")
    return hmac.new(key, domain + canonical, sha256).hexdigest()


def _sign(key: bytes, domain: bytes, value: dict) -> dict:
    return {**value, "mac": _tag(key, domain, value)}


def _verify(key: bytes, domain: bytes, value: dict) -> bool:
    tag = value.get("mac")
    if type(tag) is not str or not _MAC.fullmatch(tag):
        return False
    return hmac.compare_digest(
        _tag(key, domain, {k: v for k, v in value.items() if k != "mac"}), tag)


def new_request(registration: EdgeRegistration, after: int, limit: int = 8,
                *, nonce: str | None = None) -> dict:
    """Create an authenticated, bounded read request (random 128-bit nonce)."""
    if (not isinstance(registration, EdgeRegistration) or
            type(after) is not int or not 0 <= after <= _CURSOR_MAX or
            type(limit) is not int or not 1 <= limit <= 8):
        raise ValueError("invalid request")
    value_nonce = secrets.token_hex(16) if nonce is None else nonce
    if type(value_nonce) is not str or not _NONCE.fullmatch(value_nonce):
        raise ValueError("invalid nonce")
    return _sign(registration.transport_key, _REQ_DOMAIN, {
        "v": 1, "edge": registration.edge_id,
        "after": after, "limit": limit, "nonce": value_nonce,
    })


def get_page(outbox: BackendReceiptOutbox,
             registrations: Mapping[str, EdgeRegistration],
             request: object) -> PageResult:
    """Host transport adapter. Authenticated scope is server-owned.

    In a real HTTP integration the registry must be backed by revocable,
    account/installation-scoped enrollment, not a shared global bearer.
    """
    if (type(request) is not dict or
            set(request) != {"v", "edge", "after", "limit", "nonce", "mac"} or
            type(request.get("v")) is not int or request["v"] != 1 or
            type(request.get("edge")) is not str or
            not _EDGE.fullmatch(request["edge"]) or
            type(request.get("after")) is not int or
            not 0 <= request["after"] <= _CURSOR_MAX or
            type(request.get("limit")) is not int or
            not 1 <= request["limit"] <= 8 or
            type(request.get("nonce")) is not str or
            not _NONCE.fullmatch(request["nonce"])):
        return PageResult(PageState.INVALID)
    registration = registrations.get(request["edge"])
    if (not isinstance(registration, EdgeRegistration) or
            registration.edge_id != request["edge"] or
            not _verify(registration.transport_key, _REQ_DOMAIN, request)):
        return PageResult(PageState.REFUSED)
    if not isinstance(outbox, BackendReceiptOutbox):
        return PageResult(PageState.UNAVAILABLE)
    page = outbox.page_for_devices(
        registration.allowed_devices, request["after"], request["limit"])
    if page.state == ReadState.REFUSED:
        return PageResult(PageState.REFUSED)
    if page.state not in (ReadState.READY, ReadState.EMPTY):
        return PageResult(PageState.UNAVAILABLE)
    docs = [{"id": item.row_id, "wire": item.wire.hex()}
            for item in page.items]
    cursor = docs[-1]["id"] if docs else request["after"]
    payload = {
        "v": 1, "edge": registration.edge_id,
        "after": request["after"], "nonce": request["nonce"],
        "cursor": cursor, "more": page.has_more, "items": docs,
    }
    return PageResult(
        PageState.READY if docs else PageState.EMPTY,
        _sign(registration.transport_key, _RESP_DOMAIN, payload))


def verify_response(registration: EdgeRegistration, request: dict,
                    response: object) -> bool:
    """Page-level transport MAC, bound to request nonce and Edge identity.

    Downstream Edge must additionally validate item scope, frame shape, cursor
    and durable SQLite commit. This does not verify the embedded Tracker HMAC.
    """
    return (
        isinstance(registration, EdgeRegistration) and
        type(request) is dict and
        set(request) == {"v", "edge", "after", "limit", "nonce", "mac"} and
        type(response) is dict and
        set(response) == {"v", "edge", "after", "nonce", "cursor", "more", "items", "mac"} and
        response.get("v") == 1 and type(response.get("v")) is int and
        response.get("edge") == registration.edge_id == request.get("edge") and
        type(response.get("after")) is int and
        response.get("after") == request.get("after") and
        response.get("nonce") == request.get("nonce") and
        type(response.get("more")) is bool and
        type(response.get("cursor")) is int and
        type(response.get("items")) is list and
        0 <= len(response["items"]) <= request["limit"] and
        _verify(registration.transport_key, _REQ_DOMAIN, request) and
        _verify(registration.transport_key, _RESP_DOMAIN, response)
    )
