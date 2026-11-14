"""KQ-05 host-only: authenticated selective Backend-DURABLE receipt candidate.

The Backend alone can authenticate/decrypt the opaque PRODUCT_SECURE record
identity. The Gateway has no K_root and cannot mint this receipt. Only clean
SQLite committed observations that were independently read back and AEAD-opened
can be acknowledged. This module NEVER transmits RF, releases Tracker Flash,
erases records, or claims a deployed transport security boundary.

EXPERIMENTAL WIRE, NOT PRODUCT-APPROVED:
  [v=2, type=9, flags=0, count] [device:8] [incarnation:8]
  [key_epoch:4] [sequence_1..N:4 each] [HMAC-SHA256 truncated:16]
  N=1..8; total 44..72 bytes. Sorted identities, NO cumulative floor.
  HKDF-SHA256(salt=credential_id, IKM=K_root,
      info=b"ORUN-KQ05-BATCH-KEY-V1" + device + epoch_be32) -> 32-byte key
  MAC covers b"ORUN-KQ05-BACKEND-DURABLE-V1\\x00" + entire unsigned wire.

Replay of an identical valid receipt is harmless ONLY when incarnation and
logical record IDs are never reused and the Tracker release owner rechecks
current retained Flash. Production A2D replay/counter and credential lifecycle
remain explicit integration blockers. Do not use raw signed bytes as a release.
"""
from __future__ import annotations

from contextlib import closing
from dataclasses import dataclass
from enum import Enum, auto
import hmac
from hashlib import sha256
import sqlite3
import struct
from typing import Sequence

from cryptography.hazmat.primitives import hashes
from cryptography.hazmat.primitives.kdf.hkdf import HKDF

from product_secure_verified_ledger import (
    BackendCredential, Reject, VerifiedLocalLedger, open_product_frame,
)

_VERSION = 2
_TYPE = 9    # experimental; requires protocol registry review before firmware
_MAX = 8
_HEADER = struct.Struct(">BBBB8s8sI")
_DOMAIN = b"ORUN-KQ05-BACKEND-DURABLE-V1\x00"
_INFO = b"ORUN-KQ05-BATCH-KEY-V1"
_TAG_SIZE = 16


class State(Enum):
    SIGNED = auto()
    INVALID = auto()
    NOT_DURABLE = auto()
    UNAVAILABLE = auto()


@dataclass(frozen=True)
class Result:
    state: State
    wire: bytes = b""


def _valid_identity(device: bytes, incarnation: bytes, epoch: int,
                    sequences: Sequence[int]) -> bool:
    return (type(device) is bytes and len(device) == 8 and any(device) and
            type(incarnation) is bytes and len(incarnation) == 8 and any(incarnation) and
            type(epoch) is int and 0 <= epoch < 0xFFFFFFFF and
            type(sequences) in (tuple, list) and 1 <= len(sequences) <= _MAX and
            all(type(s) is int and 0 < s <= 0xFFFFFFFF for s in sequences) and
            all(a < b for a, b in zip(sequences, sequences[1:])))


def _key(cred: BackendCredential, device: bytes, epoch: int) -> bytes:
    return HKDF(
        algorithm=hashes.SHA256(), length=32, salt=cred.credential_id,
        info=_INFO + device + struct.pack(">I", epoch),
    ).derive(cred.k_root)


class BackendBatchIssuer:
    """Only the verified Backend ledger may issue this selective receipt.

    One read transaction checks the exact requested set, unresolved conflict
    quarantine, canonical stored identity, and real AEAD. An unrelated or
    missing record makes the entire requested batch unavailable, not a
    misleading success with an implicit contiguous range.
    """

    def __init__(self, ledger: VerifiedLocalLedger):
        if not isinstance(ledger, VerifiedLocalLedger):
            raise ValueError("verified ledger required")
        self.ledger = ledger

    def issue(self, device: bytes, incarnation: bytes, epoch: int,
              sequences: Sequence[int]) -> Result:
        if not _valid_identity(device, incarnation, epoch, sequences):
            return Result(State.INVALID)
        cred = self.ledger.credentials(device, epoch)
        if not isinstance(cred, BackendCredential):
            return Result(State.UNAVAILABLE)
        try:
            with closing(self.ledger._conn()) as db:
                # One SQLite snapshot; no partial batch under concurrent writes.
                db.execute("BEGIN")
                for seq in sequences:
                    conflict = db.execute(
                        "SELECT 1 FROM verified_conflicts WHERE "
                        "device_id=? AND incarnation=? AND sequence=? LIMIT 1",
                        (device, incarnation, seq),
                    ).fetchone()
                    if conflict:
                        return Result(State.NOT_DURABLE)
                    row = db.execute(
                        "SELECT family,plaintext,exact_first_frame "
                        "FROM verified_observations WHERE "
                        "device_id=? AND incarnation=? AND sequence=?",
                        (device, incarnation, seq),
                    ).fetchone()
                    if row is None:
                        return Result(State.NOT_DURABLE)
                    opened = open_product_frame(bytes(row[2]), self.ledger.credentials)
                    if (opened.device_id != device or
                            opened.incarnation != incarnation or
                            opened.sequence != seq or
                            opened.family != int(row[0]) or
                            opened.plaintext != bytes(row[1])):
                        return Result(State.UNAVAILABLE)
            prefix = _HEADER.pack(
                _VERSION, _TYPE, 0, len(sequences), device, incarnation, epoch)
            raw = prefix + b"".join(struct.pack(">I", seq) for seq in sequences)
            tag = hmac.new(_key(cred, device, epoch), _DOMAIN + raw, sha256).digest()[:_TAG_SIZE]
            return Result(State.SIGNED, raw + tag)
        except (sqlite3.Error, OSError, Reject, TypeError, ValueError):
            return Result(State.UNAVAILABLE)


def verify_for_tracker(wire: bytes, cred: BackendCredential,
                       device: bytes, incarnation: bytes,
                       key_epoch: int) -> tuple[int, ...] | None:
    """Host reference for a future CC310-backed Tracker security owner.

    This returns identities ONLY. Caller must independently check credential
    lifetime, replay policy, retained Flash and durable marker commit before
    considering any sequence delivered. None of that is implemented here.
    """
    if (not isinstance(wire, bytes) or
            not isinstance(cred, BackendCredential) or
            len(wire) < _HEADER.size + 4 + _TAG_SIZE):
        return None
    try:
        version, kind, flags, count, target, inc, epoch = _HEADER.unpack_from(wire)
    except struct.error:
        return None
    if (version, kind, flags) != (_VERSION, _TYPE, 0) or not 1 <= count <= _MAX:
        return None
    if len(wire) != _HEADER.size + count * 4 + _TAG_SIZE:
        return None
    sequences = struct.unpack_from(">" + "I" * count, wire, _HEADER.size)
    if not _valid_identity(target, inc, epoch, sequences):
        return None
    if not _valid_identity(device, incarnation, key_epoch, sequences):
        return None
    if target != device or inc != incarnation or epoch != key_epoch:
        return None
    tag = hmac.new(
        _key(cred, target, epoch), _DOMAIN + wire[:-_TAG_SIZE], sha256
    ).digest()[:_TAG_SIZE]
    if not hmac.compare_digest(tag, wire[-_TAG_SIZE:]):
        return None
    return sequences
