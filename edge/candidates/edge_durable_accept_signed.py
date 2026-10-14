"""SF5I host Edge durable-accept signed candidate: real Ed25519, no runtime ACK.

BINARY RECEIPT FORMAT IS EXPERIMENTAL AND NOT FROZEN/PRODUCTION-APPROVED.
Edge alone owns the private key; Gateway verifier has ONLY its enrolled
Edge public key and expected pairing/generation. No tracker K_root, backend
key, Gateway custody-ACK key or durable cleanup capability is present.

Signing follows SQLite COMMIT + independently reopened readback + complete
FIFO-table integrity scan. These host checks are not physical power-cut proof.
No radio service, network endpoint, tracker ACK, erase or reclaim is invoked.
"""
from __future__ import annotations

from dataclasses import dataclass
from enum import Enum, auto
from hashlib import sha256
import struct

from cryptography.exceptions import InvalidSignature
from cryptography.hazmat.primitives.asymmetric.ed25519 import (
    Ed25519PrivateKey, Ed25519PublicKey,
)

from edge_durable_inbox import (
    EdgeDurableInbox, PutState, ScanState, admissible_product_frame,
)

# Fixed-length unsigned experimental receipt claim; no final wire assignment.
# magic[8], version, scope, edge_id, gw_instance, attempt_id, page, slot,
# source_generation, edge_key_epoch, pairing_grant_generation,
# exact_frame_length, SHA256(exact_frame).
_FMT = ">8sBBQQQHHQIIH32s"
_MAGIC = b"ORNEA001"
_DOMAIN = b"ORUN-SF5I-EDGE-DURABLE-ACCEPT-CANDIDATE-V1\x00"
_PREFIX_SIZE = struct.calcsize(_FMT)
_SIGNATURE_SIZE = 64
_RECEIPT_SIZE = _PREFIX_SIZE + _SIGNATURE_SIZE
_SCOPE_DURABLE = 1
_MAX_U64 = (1 << 64) - 1
_MAX_U32 = (1 << 32) - 1


@dataclass(frozen=True)
class PendingCustody:
    gateway_instance: int
    attempt_id: int
    edge_issuer: int
    source_page: int
    source_slot: int
    source_generation: int
    exact_frame: bytes
    local_fallback_started: bool = False

    def admissible(self) -> bool:
        return (type(self.gateway_instance) is int and
                0 < self.gateway_instance <= _MAX_U64 and
                type(self.attempt_id) is int and
                0 < self.attempt_id <= _MAX_U64 and
                type(self.edge_issuer) is int and
                0 < self.edge_issuer <= _MAX_U64 and
                type(self.source_page) is int and 0 <= self.source_page < 32 and
                type(self.source_slot) is int and 0 <= self.source_slot < 32 and
                type(self.source_generation) is int and
                0 < self.source_generation < _MAX_U64 and
                type(self.exact_frame) is bytes and
                admissible_product_frame(self.exact_frame))


@dataclass(frozen=True)
class EdgeEnrollment:
    edge_issuer: int
    gateway_instance: int
    edge_key_epoch: int
    grant_generation: int

    def admissible(self) -> bool:
        return (type(self.edge_issuer) is int and
                0 < self.edge_issuer <= _MAX_U64 and
                type(self.gateway_instance) is int and
                0 < self.gateway_instance <= _MAX_U64 and
                type(self.edge_key_epoch) is int and
                0 < self.edge_key_epoch < _MAX_U32 and
                type(self.grant_generation) is int and
                0 < self.grant_generation < _MAX_U32)


class SignState(Enum):
    SIGNED_CANDIDATE = auto()  # Cryptographic test proof, NOT Gateway/Tracker ACK
    REFUSED = auto()
    STORE_UNCERTAIN = auto()


@dataclass(frozen=True)
class SignResult:
    state: SignState
    receipt: bytes = b""


def _prefix(p: PendingCustody, enrollment: EdgeEnrollment) -> bytes:
    return struct.pack(
        _FMT, _MAGIC, 1, _SCOPE_DURABLE, enrollment.edge_issuer,
        enrollment.gateway_instance, p.attempt_id, p.source_page,
        p.source_slot, p.source_generation, enrollment.edge_key_epoch,
        enrollment.grant_generation, len(p.exact_frame),
        sha256(p.exact_frame).digest(),
    )


class EdgeDurableAcceptSigner:
    """Authenticated candidate can only be created after Edge SQLite success.

    Caller transport/pairing authorization and Ed25519 private-key protection
    remain external production prerequisites; this API itself cannot
    authenticate the requester's network/session identity.
    """

    def __init__(self, store: EdgeDurableInbox, enrollment: EdgeEnrollment,
                 signing_key: Ed25519PrivateKey):
        if not enrollment.admissible() or not isinstance(signing_key, Ed25519PrivateKey):
            raise ValueError("invalid signed Edge enrollment")
        self.store = store
        self.enrollment = enrollment
        self.signing_key = signing_key

    def accept(self, pending: PendingCustody, *, fault: str | None = None) -> SignResult:
        if (not isinstance(pending, PendingCustody) or
                not pending.admissible() or
                pending.local_fallback_started or
                pending.edge_issuer != self.enrollment.edge_issuer or
                pending.gateway_instance != self.enrollment.gateway_instance):
            return SignResult(SignState.REFUSED)
        # No private key use until a disk-backed exact ciphertext is committed
        # and successfully read back. Uncertainty and full capacity withhold.
        result = self.store.put(pending.exact_frame, fault=fault)
        if result.state not in (PutState.COMMITTED, PutState.DUPLICATE):
            return SignResult(SignState.STORE_UNCERTAIN)
        # Existing unrelated corrupt entries also deny new receipts.
        if (self.store.oldest_batch(limit=1).state != ScanState.READY or
                result.sequence is None or
                not self.store._readback(result.sequence, pending.exact_frame)):
            return SignResult(SignState.STORE_UNCERTAIN)
        signed = _prefix(pending, self.enrollment)
        return SignResult(SignState.SIGNED_CANDIDATE,
                          signed + self.signing_key.sign(_DOMAIN + signed))


class GatewayEdgeReceiptVerifier:
    """Read-only asymmetric verifier; private key is absent at Gateway.

    Authenticated exact binding to the pending attempt and enrollment.
    A replay for the *same* idempotent attempt is allowed; uniqueness and
    reset-safe attempt identity allocation must be proved by real Gateway
    integration. This returns a cryptographic candidate classification ONLY.
    """

    def __init__(self, enrollment: EdgeEnrollment, verify_key: Ed25519PublicKey):
        if not enrollment.admissible() or not isinstance(verify_key, Ed25519PublicKey):
            raise ValueError("invalid pinned public-key enrollment")
        self.enrollment = enrollment
        self.verify_key = verify_key

    def matches(self, pending: PendingCustody, raw_receipt: bytes) -> bool:
        if (not isinstance(pending, PendingCustody) or
                not pending.admissible() or pending.local_fallback_started or
                pending.edge_issuer != self.enrollment.edge_issuer or
                pending.gateway_instance != self.enrollment.gateway_instance or
                not isinstance(raw_receipt, bytes) or
                len(raw_receipt) != _RECEIPT_SIZE):
            return False
        prefix, signature = raw_receipt[:-_SIGNATURE_SIZE], raw_receipt[-_SIGNATURE_SIZE:]
        try:
            # Byte-identical canonical claim comparison enforces ALL binding
            # fields before verifying with the specifically pinned Edge key.
            if prefix != _prefix(pending, self.enrollment):
                return False
            self.verify_key.verify(signature, _DOMAIN + prefix)
            return True
        except (InvalidSignature, ValueError, struct.error):
            return False

# No GATEWAY_CUSTODY_ACK, reclaim, backend durable accept, transport authority
# or persistent Gateway replay state in this candidate.
