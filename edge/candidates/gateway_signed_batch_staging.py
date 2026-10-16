"""SF5K2B host-only Gateway/Edge signed-proof batch selector.

Consumes the EXISTING SF5I Ed25519 receipt verifier. This is deliberately
not an ACK producer, a release capability, or an authorization service.
Neither this selection nor the Edge receipt proves Gateway custody-grant
eligibility, tracker credential lifetime or power-cut-safe nRF flash behavior.
"""
from __future__ import annotations

from dataclasses import dataclass
from enum import Enum, auto
from typing import Sequence

from edge_durable_accept_signed import GatewayEdgeReceiptVerifier, PendingCustody

_MAX_U64 = (1 << 64) - 1
_MAX_U32 = (1 << 32) - 1
_MAX_WINDOW = 4


@dataclass(frozen=True)
class CandidateScope:
    tracker_id: int
    product_key_epoch: int
    gateway_instance: int
    edge_issuer: int

    def admissible(self) -> bool:
        return (type(self.tracker_id) is int and 0 < self.tracker_id <= _MAX_U64 and
                type(self.product_key_epoch) is int and
                0 <= self.product_key_epoch < _MAX_U32 and
                type(self.gateway_instance) is int and
                0 < self.gateway_instance <= _MAX_U64 and
                type(self.edge_issuer) is int and
                0 < self.edge_issuer <= _MAX_U64)


@dataclass(frozen=True)
class ProposedMember:
    input_index: int
    exact_frame: bytes


class StageState(Enum):
    INVALID = auto()
    NO_SIGNED_DURABLE_EVIDENCE = auto()
    CANDIDATES_ONLY = auto()


@dataclass(frozen=True)
class StageResult:
    state: StageState
    members: tuple[ProposedMember, ...] = ()


def stage_signed_edge_candidates(scope: CandidateScope,
                                 pending: Sequence[PendingCustody],
                                 raw_receipts: Sequence[bytes | None],
                                 verifier: GatewayEdgeReceiptVerifier) -> StageResult:
    """Select exact members with independently verified Edge durability.

    Fail the WHOLE proposed window on malformed identities, malformed input,
    repeated exact object, attempt or source flash slot. Valid but unsigned,
    unavailable, tampered, expired, or late Edge evidence is excluded only for
    its own object. No ACK signing, flash mutation or release occurs.
    """
    invalid = StageResult(StageState.INVALID)
    if (not isinstance(scope, CandidateScope) or not scope.admissible() or
            not isinstance(verifier, GatewayEdgeReceiptVerifier) or
            not isinstance(pending, (tuple, list)) or
            not isinstance(raw_receipts, (tuple, list)) or
            not 1 <= len(pending) <= _MAX_WINDOW or
            len(pending) != len(raw_receipts)):
        return invalid
    enrolled = verifier.enrollment
    if (scope.gateway_instance != enrolled.gateway_instance or
            scope.edge_issuer != enrolled.edge_issuer):
        return invalid

    seen_exact: set[bytes] = set()
    seen_attempts: set[int] = set()
    seen_sources: set[tuple[int, int, int]] = set()
    for item in pending:
        if (not isinstance(item, PendingCustody) or not item.admissible() or
                item.gateway_instance != scope.gateway_instance or
                item.edge_issuer != scope.edge_issuer or
                int.from_bytes(item.exact_frame[8:16], "big") != scope.tracker_id or
                int.from_bytes(item.exact_frame[16:20], "big") != scope.product_key_epoch):
            return invalid
        source = (item.source_page, item.source_slot, item.source_generation)
        if (item.exact_frame in seen_exact or
                item.attempt_id in seen_attempts or source in seen_sources):
            return invalid
        seen_exact.add(item.exact_frame)
        seen_attempts.add(item.attempt_id)
        seen_sources.add(source)

    selected = []
    for index, (item, receipt) in enumerate(zip(pending, raw_receipts)):
        # No soft proof: a socket/BLE/HTTP ACK cannot pass Ed25519 validation.
        # SF5I refuses late Edge evidence after local fallback has started.
        if isinstance(receipt, bytes) and verifier.matches(item, receipt):
            selected.append(ProposedMember(index, bytes(item.exact_frame)))

    if not selected:
        return StageResult(StageState.NO_SIGNED_DURABLE_EVIDENCE)
    return StageResult(StageState.CANDIDATES_ONLY, tuple(selected))
