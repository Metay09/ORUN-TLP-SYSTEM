"""SF5H candidate: real SF5B PRODUCT_SECURE AEAD open + local verified ledger.

This module belongs ONLY on the trusted/backend side, NEVER on Gateway,
phone, or tracker. Do not commit live credentials. No authenticated transport,
account authorization, BackendDurableReceipt, ACK or data deletion exists.

Depends on the audited standard 'cryptography' package (AESCCM/HKDF).
Only 84/100-byte custody-requested SF5B DEVICE_D2A frames are admitted.
"""
from __future__ import annotations

from contextlib import closing
from dataclasses import dataclass
from enum import Enum, auto
from hashlib import sha256
from pathlib import Path
import sqlite3
from typing import Callable, Optional

from cryptography.exceptions import InvalidTag
from cryptography.hazmat.primitives import hashes
from cryptography.hazmat.primitives.ciphers.aead import AESCCM
from cryptography.hazmat.primitives.kdf.hkdf import HKDF


class Reject(Exception):
    """Untrusted/unsupported frame, bad AEAD tag or noncanonical plaintext."""


@dataclass(frozen=True)
class BackendCredential:
    credential_id: bytes
    k_root: bytes

    def __post_init__(self) -> None:
        if len(self.credential_id) != 16 or len(self.k_root) != 32:
            raise ValueError("bad credential material lengths")


@dataclass(frozen=True)
class VerifiedObservation:
    device_id: bytes  # authenticated 8-byte header identity, not a DB integer
    incarnation: bytes  # authenticated 8-byte logical record identity
    sequence: int
    family: int
    plaintext: bytes
    protected_frame: bytes


CredentialLookup = Callable[[bytes, int], Optional[BackendCredential]]


def _u(b: bytes) -> int:
    return int.from_bytes(b, "big")


def _i(b: bytes) -> int:
    return int.from_bytes(b, "big", signed=True)


def _location(source: int, lat: int, lon: int) -> bool:
    return 1 <= source <= 4 and -900000000 <= lat <= 900000000 and -1800000000 <= lon <= 1800000000


def _periodic_valid(p: bytes) -> bool:
    if len(p) != 64 or p[0] != 1 or p[2] > 4 or p[3] > 3 or p[41] > 2 or p[42] > 2:
        return False
    flags, source, battery_state = p[1], p[2], p[3]
    if not _u(p[4:12]) or not _u(p[12:16]):
        return False
    end, duration = _u(p[16:20]), _u(p[20:24])
    if not duration or bool(flags & 1) != bool(end):
        return False
    lat, lon, alt = _i(p[24:28]), _i(p[28:32]), _i(p[32:36])
    hdop, age, sats = _u(p[36:38]), _u(p[38:40]), p[40]
    loc, altitude, age_valid = bool(flags & 2), bool(flags & 4), bool(flags & 8)
    if not loc:
        if source or altitude or age_valid or lat or lon or alt or hdop or sats or age != 65535:
            return False
    else:
        if not _location(source, lat, lon) or (not altitude and alt):
            return False
        if age_valid and age > 65534 or not age_valid and age != 65535:
            return False
        if source != 1 and (hdop or sats):
            return False
    covered, active = _u(p[44:48]), _u(p[48:52])
    if not (flags & 16):
        if covered or active or _u(p[52:54]) or _u(p[54:56]) or p[41]:
            return False
    else:
        if not covered or active > covered or covered > duration:
            return False
        if p[41] != (2 if covered == duration else 1):
            return False
    if flags & 128:
        if not (flags & 16) or p[43] > 200 or (not active and p[43]):
            return False
    elif p[43]:
        return False
    if not (flags & 32) and (_u(p[56:58]) or battery_state or p[42]):
        return False
    if _u(p[58:60]) & ~0x07FF:
        return False
    if not (flags & 64) and (_u(p[58:60]) or any(p[60:64])):
        return False
    return True


def _event_valid(p: bytes) -> bool:
    if len(p) != 48 or p[0] != 1:
        return False
    family, lifecycle, severity, flags, reason, source, context = p[1:8]
    if not (1 <= family <= 4 and 1 <= lifecycle <= 2 and
            1 <= severity <= 3 and flags <= 15 and source <= 4 and context <= 2):
        return False
    if (family == 3 and reason not in (1, 2)) or (family != 3 and reason != 0):
        return False
    if not _u(p[8:16]) or not _u(p[16:20]) or not _u(p[24:32]):
        return False
    if bool(flags & 1) != bool(_u(p[20:24])):
        return False
    lat, lon, age = _i(p[32:36]), _i(p[36:40]), _u(p[40:42])
    if not flags & 2:
        if source or (flags & 4) or lat or lon or age != 65535:
            return False
    else:
        if not _location(source, lat, lon):
            return False
        if flags & 4 and age > 65534 or not flags & 4 and age != 65535:
            return False
    if _u(p[42:44]):
        return False
    if flags & 8:
        if context == 0:
            return False
    elif context or _u(p[44:48]):
        return False
    return True


def open_product_frame(raw: bytes, get_credential: CredentialLookup) -> VerifiedObservation:
    """Authenticates complete SF5B AAD+ciphertext+tag before trusting identity.

    Returns only decrypted/semantically checked data. The caller must still
    enforce account authorization, credential validity and custody policy.
    """
    if not isinstance(raw, bytes) or len(raw) not in (84, 100):
        raise Reject("unsupported length")
    header = raw[:28]
    if (header[0:3] != bytes((2, 4, 2)) or header[7] != 0 or
            header[5] != 1 or header[4] not in (2, 3) or
            (header[3], header[6], len(raw)) not in ((1, 64, 100), (2, 48, 84))):
        raise Reject("invalid protected header")
    device = header[8:16]
    epoch = _u(header[16:20])
    counter = _u(header[20:28])
    if epoch == 0xFFFFFFFF or counter == 0:
        raise Reject("invalid epoch/counter")
    credential = get_credential(device, epoch)
    if credential is None:
        raise Reject("unknown device/epoch")
    if not isinstance(credential, BackendCredential):
        raise Reject("invalid credentials provider")
    info = b"ORUN-TLP-V2-AEAD" + bytes((1,)) + header[16:20]
    traffic_key = HKDF(
        algorithm=hashes.SHA256(), length=16,
        salt=credential.credential_id, info=info,
    ).derive(credential.k_root)
    nonce = header[16:20] + bytes((1,)) + header[20:28]
    try:
        plaintext = AESCCM(traffic_key, tag_length=8).decrypt(nonce, raw[28:], header)
    except (InvalidTag, ValueError) as exc:
        raise Reject("AEAD authentication failed") from exc
    family = header[3]
    if not (_periodic_valid(plaintext) if family == 1 else _event_valid(plaintext)):
        raise Reject("noncanonical authenticated product data")
    inc_offset = 4 if family == 1 else 8
    seq_offset = 12 if family == 1 else 16
    return VerifiedObservation(
        device, plaintext[inc_offset:inc_offset + 8],
        _u(plaintext[seq_offset:seq_offset + 4]), family, plaintext, raw,
    )


class LedgerState(Enum):
    VERIFIED_STORED = auto()       # test host SQLite only, NOT BackendDurableReceipt
    VERIFIED_DUPLICATE = auto()    # same logical identity and exact canonical content
    CONTENT_CONFLICT = auto()      # identity reused with divergent family/plaintext
    REJECTED = auto()              # untrusted / malformed / unknown credential
    UNCERTAIN = auto()             # SQLite/readback/error; do not issue receipt


@dataclass(frozen=True)
class LedgerResult:
    state: LedgerState


class VerifiedLocalLedger:
    """Backend-only local candidate: AEAD verified and content-aware dedup.

    No app/account authorization, credential retirement, signed receipt, async
    network trust, release/delete, or production power-loss proof.
    """

    def __init__(self, db_path: str | Path, credentials: CredentialLookup,
                 max_conflicts: int = 4096):
        path = str(db_path)
        if (not path or path == ":memory:" or path.startswith("file:") or
                type(max_conflicts) is not int or not 1 <= max_conflicts <= 100000):
            raise ValueError("disk SQLite path and bounded conflict capacity required")
        self.path = path
        self.credentials = credentials
        self.max_conflicts = max_conflicts
        with closing(self._conn()) as db:
            if db.execute("PRAGMA journal_mode=WAL").fetchone()[0].lower() != "wal":
                raise RuntimeError("SQLite WAL mode required")
            db.execute(
                "CREATE TABLE IF NOT EXISTS verified_observations ("
                " device_id BLOB NOT NULL CHECK(length(device_id)=8),"
                " incarnation BLOB NOT NULL CHECK(length(incarnation)=8),"
                " sequence INTEGER NOT NULL CHECK(sequence>0),"
                " family INTEGER NOT NULL CHECK(family IN (1,2)),"
                " plaintext BLOB NOT NULL CHECK(length(plaintext) IN (48,64)),"
                " exact_first_frame BLOB NOT NULL CHECK(length(exact_first_frame) IN (84,100)),"
                " PRIMARY KEY (device_id,incarnation,sequence))"
            )
            # A verified logical-identity conflict is NOT discarded.
            # Retain the first exact protected evidence for each distinct
            # conflicting canonical content. Never overwrite the winner.
            db.execute(
                "CREATE TABLE IF NOT EXISTS verified_conflicts ("
                " device_id BLOB NOT NULL CHECK(length(device_id)=8),"
                " incarnation BLOB NOT NULL CHECK(length(incarnation)=8),"
                " sequence INTEGER NOT NULL CHECK(sequence>0),"
                " family INTEGER NOT NULL CHECK(family IN (1,2)),"
                " plaintext BLOB NOT NULL CHECK(length(plaintext) IN (48,64)),"
                " exact_first_frame BLOB NOT NULL CHECK(length(exact_first_frame) IN (84,100)),"
                " PRIMARY KEY (device_id,incarnation,sequence,family,plaintext))"
            )

    def _conn(self) -> sqlite3.Connection:
        db = sqlite3.connect(self.path, timeout=1.0, isolation_level=None)
        db.execute("PRAGMA busy_timeout=1000")
        db.execute("PRAGMA synchronous=FULL")
        return db

    def _matches(self, item: VerifiedObservation, conflict: bool = False) -> bool:
        # Reopen after COMMIT, and verify the stored FIRST exact ciphertext
        # really authenticates back to the claimed logical content. A damaged
        # local DB is not allowed to yield a duplicate success.
        table = "verified_conflicts" if conflict else "verified_observations"
        with closing(self._conn()) as db:
            query = ("SELECT family,plaintext,exact_first_frame FROM " + table +
                     " WHERE device_id=? AND incarnation=? AND sequence=?")
            args: tuple[object, ...] = (item.device_id, item.incarnation, item.sequence)
            if conflict:
                query += " AND family=? AND plaintext=?"
                args += (item.family, item.plaintext)
            saved = db.execute(query, args).fetchone()
            if (saved is None or saved[0] != item.family or
                    bytes(saved[1]) != item.plaintext):
                return False
            try:
                stored = open_product_frame(bytes(saved[2]), self.credentials)
            except (Reject, TypeError, ValueError):
                return False
            return (stored.device_id == item.device_id and
                    stored.incarnation == item.incarnation and
                    stored.sequence == item.sequence and
                    stored.family == item.family and
                    stored.plaintext == item.plaintext)

    def ingest(self, raw: bytes) -> LedgerResult:
        try:
            item = open_product_frame(raw, self.credentials)
        except Reject:
            return LedgerResult(LedgerState.REJECTED)
        try:
            with closing(self._conn()) as db:
                db.execute("BEGIN IMMEDIATE")
                conflict_evidence = False
                try:
                    saved = db.execute(
                        "SELECT family,plaintext FROM verified_observations"
                        " WHERE device_id=? AND incarnation=? AND sequence=?",
                        (item.device_id, item.incarnation, item.sequence),
                    ).fetchone()
                    if saved is None:
                        db.execute(
                            "INSERT INTO verified_observations"
                            " (device_id,incarnation,sequence,family,plaintext,exact_first_frame)"
                            " VALUES(?,?,?,?,?,?)",
                            (item.device_id, item.incarnation, item.sequence,
                             item.family, item.plaintext, item.protected_frame),
                        )
                        state = LedgerState.VERIFIED_STORED
                    elif int(saved[0]) == item.family and bytes(saved[1]) == item.plaintext:
                        # An unresolved conflicting observation quarantines
                        # the whole logical identity, even the original winner.
                        # Never report a clean duplicate while a divergence
                        # is known, or future receipt code could release it.
                        has_conflict = db.execute(
                            "SELECT 1 FROM verified_conflicts"
                            " WHERE device_id=? AND incarnation=? AND sequence=? LIMIT 1",
                            (item.device_id, item.incarnation, item.sequence),
                        ).fetchone()
                        state = (LedgerState.CONTENT_CONFLICT if has_conflict
                                 else LedgerState.VERIFIED_DUPLICATE)
                    else:
                        # Persist the conflicting authenticated evidence
                        # transactionally. Replays of identical conflict
                        # content do not multiply forensic rows.
                        prior = db.execute(
                            "SELECT exact_first_frame FROM verified_conflicts"
                            " WHERE device_id=? AND incarnation=? AND sequence=?"
                            " AND family=? AND plaintext=?",
                            (item.device_id, item.incarnation, item.sequence,
                             item.family, item.plaintext),
                        ).fetchone()
                        if prior is None:
                            count = int(db.execute(
                                "SELECT count(*) FROM verified_conflicts"
                            ).fetchone()[0])
                            if count >= self.max_conflicts:
                                db.execute("ROLLBACK")
                                return LedgerResult(LedgerState.UNCERTAIN)
                            db.execute(
                                "INSERT INTO verified_conflicts"
                                " (device_id,incarnation,sequence,family,plaintext,exact_first_frame)"
                                " VALUES(?,?,?,?,?,?)",
                                (item.device_id, item.incarnation, item.sequence,
                                 item.family, item.plaintext, item.protected_frame),
                            )
                        state = LedgerState.CONTENT_CONFLICT
                        conflict_evidence = True
                    db.execute("COMMIT")
                except Exception:
                    if db.in_transaction:
                        db.execute("ROLLBACK")
                    raise
            if not self._matches(item, conflict=conflict_evidence):
                return LedgerResult(LedgerState.UNCERTAIN)
            return LedgerResult(state)
        except (sqlite3.Error, OSError):
            return LedgerResult(LedgerState.UNCERTAIN)

    def conflict_count(self) -> Optional[int]:
        try:
            with closing(self._conn()) as db:
                return int(db.execute(
                    "SELECT count(*) FROM verified_conflicts"
                ).fetchone()[0])
        except sqlite3.Error:
            return None

    def record_count(self) -> Optional[int]:
        try:
            with closing(self._conn()) as db:
                return int(db.execute("SELECT count(*) FROM verified_observations").fetchone()[0])
        except sqlite3.Error:
            return None

# Intentional omissions: receipts, backend delivery assertion, release, erase,
# secret storage, phone/Gateway key delivery and public HTTP listen APIs.
