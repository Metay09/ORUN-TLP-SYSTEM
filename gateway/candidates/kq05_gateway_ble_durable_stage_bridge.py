"""KQ05 host-only Gateway BLE logical-message -> durable receipt staging.

This is a *reference integration seam*, not RAK firmware or a public endpoint.
The real firmware still refuses 0x91 Edge offers until trusted installation
enrollment and a non-overlapping persistent Gateway receipt owner exist.

0x91 wraps the existing authenticated Edge/Gateway offer; logical BLE 0x08
wraps the authenticated inner 0x92 STAGED result. Only the existing GatewayReceiptStage SQLite
transaction + readback may authorize 0x92. Neither status authorizes Tracker
release, Backend deletion, custody ACK, or RF transmission.

The two booleans model independent Gateway-service and Edge-installation
authorization at the integration boundary. They are NOT authentication: no
production adapter may populate them from a BLE connection or caller bytes.
"""
from __future__ import annotations

from dataclasses import dataclass
from enum import Enum, auto

from kq05_edge_gateway_offer import verify_offer
from kq05_gateway_receipt_stage import GatewayReceiptStage
from kq05_gateway_stage_ack import (
    ExchangeState, gateway_accept_and_reply,
)

_OFFER = 0x91
_STAGED = 0x08  # SF5L6 BLE envelope; authenticated inner response type remains 0x92
_MIN_OFFER = 96
_MAX_OFFER = 124
_STAGE_BYTES = 84


class BridgeState(Enum):
    STAGED = auto()   # gateway-local durable receipt; NEVER Tracker delivery
    RETRY = auto()    # full/unavailable/readback failure; no response
    REFUSED = auto()  # unauthorized or malformed; no response


@dataclass(frozen=True)
class BridgeResult:
    state: BridgeState
    response: bytes = b""


class GatewayBleDurableStageBridge:
    """Bounded synchronous host adapter. No RF, device flash, or deletes."""

    def __init__(self, stage: GatewayReceiptStage):
        if not isinstance(stage, GatewayReceiptStage):
            raise ValueError("durable Gateway staging owner required")
        self._stage = stage

    def receive(self, logical: object, *,
                authorized_gateway_service: bool,
                verified_edge_installation: bool) -> BridgeResult:
        # The actual RAK adapter must derive both authorities independently;
        # this class does not and cannot enroll a trusted installation.
        if (authorized_gateway_service is not True or
                verified_edge_installation is not True):
            return BridgeResult(BridgeState.REFUSED)

        if (type(logical) is not bytes or
                not _MIN_OFFER <= len(logical) <= _MAX_OFFER or
                logical[0] != _OFFER):
            return BridgeResult(BridgeState.REFUSED)

        inner = logical[1:]
        if verify_offer(inner, self._stage.permit) is None:
            return BridgeResult(BridgeState.REFUSED)

        result = gateway_accept_and_reply(self._stage, inner)
        if result.state == ExchangeState.RETRY:
            return BridgeResult(BridgeState.RETRY)
        if result.state != ExchangeState.CONFIRMED:
            return BridgeResult(BridgeState.REFUSED)

        answer = bytes([_STAGED]) + result.response
        if len(answer) != _STAGE_BYTES:
            # No successful STAGED status if the wire contract drifts.
            return BridgeResult(BridgeState.RETRY)
        return BridgeResult(BridgeState.STAGED, answer)
