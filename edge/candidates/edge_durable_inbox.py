"""SF5H host-only Edge SQLite inbox for *opaque exact* PRODUCT_SECURE frames.

Transport-neutral storage candidate, not an authenticated Edge endpoint.
It generates no EDGE_DURABLE_ACCEPT or GATEWAY_CUSTODY_ACK and MUST NOT
be used as permission for Gateway reclaim. Actual cryptographic Edge
authority, protocol and physical power-cut qualification are not implemented.
"""
from __future__ import annotations

from contextlib import closing
from dataclasses import dataclass
from enum import Enum, auto
from hashlib import sha256
from pathlib import Path
import sqlite3
from typing import Literal, Optional

FaultPoint = Optional[Literal["before_commit", "after_commit"]]


class PutState(Enum):
    COMMITTED = auto()  # stored+postcommit readback, host filesystem only
    DUPLICATE = auto()  # exact original object read back after reopen
    FULL = auto()       # no room; no mutation
    INVALID = auto()    # invalid PRODUCT_SECURE shape
    UNCERTAIN = auto()  # failure/exception or indeterminate post-commit
    REJECTED = auto()   # inaccessible, corrupted, or nonconforming database


@dataclass(frozen=True)
class PutResult:
    state: PutState
    sequence: Optional[int] = None


class ScanState(Enum):
    READY = auto()      # bounded FIFO batch after complete-table integrity check
    EMPTY = auto()      # no rows in a coherent snapshot
    REFUSED = auto()    # malformed content or impossible retained metadata
    UNAVAILABLE = auto()  # SQLite busy, unreadable or otherwise uncertain
    INVALID = auto()    # bad requested batch limit


@dataclass(frozen=True)
class QueuedFrame:
    sequence: int           # local Edge row identity, NOT protocol identity
    ciphertext: bytes       # exact 84/100B protected object


@dataclass(frozen=True)
class ScanResult:
    state: ScanState
    items: tuple[QueuedFrame, ...] = ()


class InjectedFailure(Exception):
    pass


def admissible_product_frame(data: bytes) -> bool:
    """Only SF5G's 84B EVENT and 100B PERIODIC custody-requested frames.

    Shape checks are NOT AEAD verification or device authorization. RESULT
    and other protected families require explicit separate admission.
    """
    if len(data) not in (84, 100):
        return False
    if (data[0], data[1], data[2], data[5], data[7]) != (2, 4, 2, 1, 0):
        return False
    if data[4] not in (2, 3):
        return False
    if data[3] != (2 if len(data) == 84 else 1):
        return False
    if data[6] != (48 if len(data) == 84 else 64):
        return False
    if data[16:20] == bytes([0xFF]) * 4 or data[20:28] == bytes(8):
        return False
    return True


class EdgeDurableInbox:
    """Conservative, bounded, synchronous SQLite inbox (no network server).

    A transaction's successful COMMIT plus independent reopened exact-byte
    readback is modeled as host-local persistence; it is NOT proof of actual
    power-loss reliability and cannot on its own authorize a signed receipt.

    SHA-256 is an *index*, NOT authentication, identity or a unique key.
    Every duplicate must match full ciphertext bytes and length. Collisions
    are stored separately (rather than incorrectly suppressing a new object).
    """

    def __init__(self, db_path: str | Path, max_objects: int = 4096):
        path = str(db_path)
        if (not path or path == ":memory:" or path.startswith("file:") or
                max_objects < 1 or max_objects > 100000):
            raise ValueError("durable disk path and bounded max_objects required")
        self.path = path
        self.max_objects = max_objects
        self._setup()

    def _connect(self) -> sqlite3.Connection:
        connection = sqlite3.connect(self.path, timeout=1.0, isolation_level=None)
        connection.execute("PRAGMA busy_timeout=1000")
        connection.execute("PRAGMA synchronous=FULL")
        return connection

    def _setup(self) -> None:
        with closing(self._connect()) as conn:
            mode = conn.execute("PRAGMA journal_mode=WAL").fetchone()[0]
            if mode.lower() != "wal":
                raise RuntimeError("WAL journaling is required for inbox")
            conn.execute("BEGIN IMMEDIATE")
            try:
                conn.execute(
                    "CREATE TABLE IF NOT EXISTS exact_frames ("
                    " sequence INTEGER PRIMARY KEY AUTOINCREMENT,"
                    " digest BLOB NOT NULL CHECK(length(digest)=32),"
                    " size INTEGER NOT NULL CHECK(size IN (84,100)),"
                    " ciphertext BLOB NOT NULL CHECK(length(ciphertext)=size))"
                )
                conn.execute(
                    "CREATE INDEX IF NOT EXISTS exact_frames_digest_size "
                    "ON exact_frames(digest,size)"
                )
                conn.execute("COMMIT")
            except Exception:
                conn.execute("ROLLBACK")
                raise

    def _readback(self, sequence: int, original: bytes) -> bool:
        # Independently reopen the SQLite store after successful COMMIT.
        try:
            with closing(self._connect()) as conn:
                row = conn.execute(
                    "SELECT ciphertext,size,digest FROM exact_frames WHERE sequence=?",
                    (sequence,),
                ).fetchone()
                return bool(row and row[1] == len(original) and
                            bytes(row[0]) == original and
                            bytes(row[2]) == sha256(original).digest())
        except sqlite3.Error:
            return False

    def put(self, object_bytes: bytes, *,
            fault: FaultPoint = None) -> PutResult:
        if not isinstance(object_bytes, bytes) or not admissible_product_frame(object_bytes):
            return PutResult(PutState.INVALID)
        if fault not in (None, "before_commit", "after_commit"):
            return PutResult(PutState.INVALID)

        digest = sha256(object_bytes).digest()
        sequence: Optional[int] = None
        duplicate = False
        committed = False

        try:
            with closing(self._connect()) as conn:
                conn.execute("BEGIN IMMEDIATE")
                try:
                    matches = conn.execute(
                        "SELECT sequence,ciphertext FROM exact_frames "
                        "WHERE digest=? AND size=? ORDER BY sequence ASC",
                        (digest, len(object_bytes)),
                    ).fetchall()
                    for existing_id, blob in matches:
                        stored = bytes(blob)
                        if len(stored) != len(object_bytes) or sha256(stored).digest() != digest:
                            conn.execute("ROLLBACK")
                            return PutResult(PutState.REJECTED)
                        if stored == object_bytes:
                            sequence = int(existing_id)
                            duplicate = True
                            break

                    if not duplicate:
                        count = int(conn.execute(
                            "SELECT count(*) FROM exact_frames"
                        ).fetchone()[0])
                        if count >= self.max_objects:
                            conn.execute("ROLLBACK")
                            return PutResult(PutState.FULL)
                        cursor = conn.execute(
                            "INSERT INTO exact_frames(digest,size,ciphertext) "
                            "VALUES (?,?,?)",
                            (digest, len(object_bytes), sqlite3.Binary(object_bytes)),
                        )
                        sequence = int(cursor.lastrowid)
                    if fault == "before_commit":
                        raise InjectedFailure("transaction interrupted")
                    conn.execute("COMMIT")
                    committed = True
                except Exception:
                    if conn.in_transaction:
                        conn.execute("ROLLBACK")
                    raise
            # "after_commit" represents a lost response/power interruption
            # after durable commit but before local verification. The caller
            # is required to retry; exact duplicate will be recognized.
            if fault == "after_commit":
                return PutResult(PutState.UNCERTAIN)
            if sequence is None or not committed or not self._readback(sequence, object_bytes):
                return PutResult(PutState.UNCERTAIN)
            return PutResult(PutState.DUPLICATE if duplicate else PutState.COMMITTED,
                             sequence)
        except (sqlite3.Error, OSError, InjectedFailure):
            return PutResult(PutState.UNCERTAIN)

    def retained_count(self) -> Optional[int]:
        try:
            with closing(self._connect()) as conn:
                return int(conn.execute(
                    "SELECT count(*) FROM exact_frames"
                ).fetchone()[0])
        except sqlite3.Error:
            return None

    def oldest_batch(self, limit: int = 16) -> ScanResult:
        """Read a bounded oldest-first batch, validating the ENTIRE queue.

        A later corrupted object invalidates the whole batch; no partial
        earlier 'good' prefix is returned to downstream code. Uses one
        consistent SQLite read transaction. This is NOT a backend delivery
        claim, an authenticated receipt, or a checkpoint/reclaim operation.
        """
        if type(limit) is not int or limit < 1 or limit > 64:
            return ScanResult(ScanState.INVALID)
        try:
            with closing(self._connect()) as conn:
                conn.execute("BEGIN")
                items: list[QueuedFrame] = []
                rows_seen = 0
                previous_sequence = 0
                cursor = conn.execute(
                    "SELECT sequence,digest,size,ciphertext "
                    "FROM exact_frames ORDER BY sequence ASC"
                )
                for sequence, digest, size, object_blob in cursor:
                    rows_seen += 1
                    if (rows_seen > self.max_objects or
                            type(sequence) is not int or
                            sequence <= previous_sequence or sequence <= 0 or
                            type(size) is not int or size not in (84, 100) or
                            not isinstance(digest, bytes) or len(digest) != 32 or
                            not isinstance(object_blob, bytes) or
                            len(object_blob) != size or
                            not admissible_product_frame(object_blob) or
                            sha256(object_blob).digest() != digest):
                        return ScanResult(ScanState.REFUSED)
                    previous_sequence = sequence
                    if len(items) < limit:
                        items.append(QueuedFrame(sequence, object_blob))
                conn.execute("COMMIT")
                if rows_seen == 0:
                    return ScanResult(ScanState.EMPTY)
                return ScanResult(ScanState.READY, tuple(items))
        except (sqlite3.Error, OSError):
            return ScanResult(ScanState.UNAVAILABLE)

    # Intentionally no delete / reclaim / receipt-generation function.
