"""SF5K2C host TEST-ONLY proposed Gateway batch ACK oracle.

Unapproved candidate 0x06. Not a production signer, custody authority,
HKDF derivation, persistent Gateway enrollment, release or radio operation.
It consumes SF5I Ed25519 verified Edge facts, not transport callbacks.
"""
from __future__ import annotations

from dataclasses import dataclass
import hashlib
import hmac
from typing import Sequence

from gateway_signed_batch_staging import (
    CandidateScope, StageState, stage_signed_edge_candidates,
)
from edge_durable_accept_signed import GatewayEdgeReceiptVerifier, PendingCustody

_DOMAIN = b"ORUN-TLP-V2-CUSTODY-BATCH-ACK-MAC-v1"
_MAX_U32 = (1 << 32) - 1
_MAX_U64 = (1 << 64) - 1


@dataclass(frozen=True)
class TestOnlyGatewayGrant:
    """Test fixture only, NOT enrolled/persisted authority."""
    gateway_id: int
    policy_floor: int
    grant_generation: int
    active: bool = False

    def structurally_valid(self) -> bool:
        return (self.active is True and
                type(self.gateway_id) is int and 0 < self.gateway_id <= _MAX_U64 and
                type(self.policy_floor) is int and 0 <= self.policy_floor <= _MAX_U32 and
                type(self.grant_generation) is int and
                0 < self.grant_generation <= _MAX_U32)


def compose_host_fixture_ack(
    scope: CandidateScope,
    pending: Sequence[PendingCustody],
    edge_receipts: Sequence[bytes | None],
    edge_verifier: GatewayEdgeReceiptVerifier,
    grant: TestOnlyGatewayGrant,
    test_only_scoped_key: bytes,
    *,
    relay_allowed: bool = True,
) -> bytes | None:
    """Recheck signed receipts, make exact-digest-bound 0x06 fixture bytes.

    Returns None on any missing test grant or invalid window. The fixture
    deliberately has no callable path to a live Gateway or Tracker. It does
    NOT prove the key was issued by an enrolled Gateway. No ACK release API.
    """
    if (not isinstance(grant, TestOnlyGatewayGrant) or
            not grant.structurally_valid() or
            type(test_only_scoped_key) is not bytes or
            len(test_only_scoped_key) != 32 or
            type(relay_allowed) is not bool):
        return None
    staged = stage_signed_edge_candidates(scope, pending, edge_receipts,
                                          edge_verifier)
    if staged.state is not StageState.CANDIDATES_ONLY:
        return None

    # Member order is canonical by (length, SHA-256 64-bit lookup hint).
    # The MAC incorporates ALL full SHA-256 digests, not just the hints.
    entries = []
    for member in staged.members:
        obj = member.exact_frame
        whole = hashlib.sha256(obj).digest()
        entries.append((len(obj), whole[:8], whole))
    entries.sort(key=lambda e: (e[0], e[1]))
    for i in range(1, len(entries)):
        if entries[i][:2] <= entries[i - 1][:2]:
            # Duplicate or ambiguous 64-bit hint: never create a fixture.
            return None

    # Bounds are already validated for scope by SF5K2B selection. Keep this
    # defensive assertion local to a *test* interface, never a production key.
    try:
        head = (bytes((2, 6, int(relay_allowed), len(entries))) +
                scope.tracker_id.to_bytes(8, "big") +
                grant.gateway_id.to_bytes(8, "big") +
                scope.product_key_epoch.to_bytes(4, "big") +
                grant.policy_floor.to_bytes(4, "big") +
                grant.grant_generation.to_bytes(4, "big"))
        for length, hint, _ in entries:
            head += bytes((length,)) + hint
    except (ValueError, OverflowError, AttributeError):
        return None
    tag = hmac.new(test_only_scoped_key,
                   _DOMAIN + head + b"".join(whole for _, _, whole in entries),
                   hashlib.sha256).digest()[:16]
    result = head + tag
    assert len(result) == 48 + len(entries) * 9
    return result
