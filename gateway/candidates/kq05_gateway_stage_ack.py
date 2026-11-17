"""KQ05 LAB: authenticated durable-STAGED result for Edge->Gateway offers.

Only GatewayReceiptStage.accept() can produce a positive result, after its
SQLite COMMIT and independent readback. This is an Edge->Gateway *storage*
acknowledgement; it is NEVER Tracker receipt, Backend release, or legacy
CUSTODY_ACK. No production USB/BLE handler is installed.
"""
from __future__ import annotations

from dataclasses import dataclass
from enum import Enum, auto
from hashlib import sha256
import hmac
import struct

from kq05_edge_gateway_offer import (
    GatewayLinkPermit, VerifiedOffer, verify_offer,
)
from kq05_gateway_receipt_stage import GatewayReceiptStage, StageState

_ACK_TYPE = 0x92
_VERSION = 1
_SUCCESS = 1  # durable staged now
_ALREADY = 2  # identical durable staged record was present
_FMT = struct.Struct(">BB8sQB32s")
_DOMAIN = b"ORUN-KQ05-GW-EDGE-STAGED-RESULT-v1\x00"
_SIZE = _FMT.size + 32
assert _SIZE <= 160


class ExchangeState(Enum):
    CONFIRMED = auto()
    RETRY = auto()
    REFUSED = auto()


@dataclass(frozen=True)
class ExchangeResult:
    state: ExchangeState
    response: bytes = b""


@dataclass(frozen=True)
class EdgeStageConfirmation:
    source_row: int
    already_staged: bool


def _mac(prefix: bytes, permit: GatewayLinkPermit) -> bytes:
    return hmac.new(
        permit.key,
        _DOMAIN + permit.edge_id.encode("ascii") + b"\x00" + prefix,
        sha256,
    ).digest()


def gateway_accept_and_reply(
        stage: GatewayReceiptStage, offer_bytes: object) -> ExchangeResult:
    """Success only after the Gateway's original durable accept/readback.

    Deliberately suppresses an ACK for REFUSED/FULL/UNAVAILABLE; the Edge
    retains the original offer and applies bounded backoff/retry policies.
    """
    if not isinstance(stage, GatewayReceiptStage):
        return ExchangeResult(ExchangeState.REFUSED)
    offer = verify_offer(offer_bytes, stage.permit)
    if offer is None:
        return ExchangeResult(ExchangeState.REFUSED)
    result = stage.accept(offer_bytes)
    if result.state not in (StageState.STAGED, StageState.DUPLICATE):
        return ExchangeResult(
            ExchangeState.REFUSED if result.state == StageState.REFUSED
            else ExchangeState.RETRY)
    # A second independent full-scan is required before emitting an HMAC ACK.
    readback = stage.scan(limit=32)
    if readback.state != StageState.STAGED:
        return ExchangeResult(ExchangeState.RETRY)
    # Scan's result is paged; read the exact matching row even when >32 staged.
    with __import__("contextlib").closing(stage._conn()) as db:
        try:
            row = db.execute(
                "SELECT id,source_row,wire,tag FROM kq05_gateway_receipts "
                "WHERE id=?", (result.local_id,)).fetchone()
            if (row is None or row[1] != offer.source_row or
                    not hmac.compare_digest(row[2], offer.wire) or
                    not hmac.compare_digest(
                        row[3], stage._seal(offer.source_row, offer.wire))):
                return ExchangeResult(ExchangeState.RETRY)
        except Exception:
            return ExchangeResult(ExchangeState.RETRY)
    code = _ALREADY if result.state == StageState.DUPLICATE else _SUCCESS
    unsigned = _FMT.pack(
        _VERSION, _ACK_TYPE, stage.permit.gateway_id,
        offer.source_row, code, sha256(offer_bytes).digest()
    )
    return ExchangeResult(
        ExchangeState.CONFIRMED, unsigned + _mac(unsigned, stage.permit))


def edge_verify_stage_result(
        offer_bytes: object, answer: object,
        permit: GatewayLinkPermit) -> EdgeStageConfirmation | None:
    """Only acknowledges Gateway-local staging of this exact Edge offer.

    Return value MUST NOT authorize receipt eviction, Backend delivery, or
    Tracker Flash release.
    """
    if (not isinstance(permit, GatewayLinkPermit) or
            type(offer_bytes) is not bytes or
            type(answer) is not bytes or len(answer) != _SIZE):
        return None
    offer: VerifiedOffer | None = verify_offer(offer_bytes, permit)
    if offer is None:
        return None
    ver, kind, gateway, source_row, code, digest = _FMT.unpack_from(answer)
    if (ver != _VERSION or kind != _ACK_TYPE or
            gateway != permit.gateway_id or
            source_row != offer.source_row or
            code not in (_SUCCESS, _ALREADY) or
            not hmac.compare_digest(digest, sha256(offer_bytes).digest()) or
            not hmac.compare_digest(answer[-32:], _mac(answer[:-32], permit))):
        return None
    return EdgeStageConfirmation(source_row, code == _ALREADY)
