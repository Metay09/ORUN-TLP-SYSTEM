"""ORUN Step 3C candidate: owner-approved Edge enrollment and per-install HTTP credentials.

HOST PREFLIGHT ONLY. No public enrollment HTTP endpoint, no automatic owner
approval, no Gateway grant updates and no tracker read-key distribution.

Trust boundary:
- begin() accepts an UNTRUSTED installation identity and public key.
- approve() is available ONLY to an authenticated owner/operator on a separate
  future administrative channel. Do not expose it on the public sync API.
- finish() verifies both owner approval code and Ed25519 or P-256 proof of possession.
- resolve() maps a high-entropy per-installation bearer credential to a trusted
  Actor; caller-supplied site/user/permission headers are never consulted.
"""
from __future__ import annotations

from contextlib import contextmanager
from dataclasses import dataclass
import hashlib
import hmac
import json
import os
from pathlib import Path
import re
import secrets
import sqlite3
import stat
import time
from typing import FrozenSet

from cryptography.exceptions import InvalidSignature
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PublicKey
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.hazmat.primitives import hashes, serialization

from .shared_sync_store import Actor

ID = re.compile(r"^[A-Za-z0-9._-]{1,128}$")
SCOPES = frozenset({"zone:read", "zone:write", "group:read", "group:write",
                    "zone_target:read", "zone_target:write"})
CONTEXT = b"ORUN:STEP3C:EDGE-ENROLL:v1\x00"
P256_CONTEXT = b"ORUN:STEP3C:EDGE-ENROLL:P256:v2\x00"
ALGORITHMS = frozenset(("Ed25519", "P256-SHA256"))
MAX_PENDING_REQUESTS = 32
MAX_PENDING_SECONDS = 15 * 60
MAX_SESSION_SECONDS = 30 * 24 * 3600


class EnrollmentError(ValueError):
    pass


class EnrollmentRefused(EnrollmentError):
    pass


def _id(value: str) -> str:
    if not isinstance(value, str) or not ID.fullmatch(value):
        raise EnrollmentError("invalid identity")
    return value


def _bytes32(value: bytes) -> bytes:
    if not isinstance(value, bytes) or len(value) != 32:
        raise EnrollmentError("invalid 32-byte Ed25519 key")
    return value


def _validated_public_key(value: bytes, algorithm: str) -> bytes:
    if algorithm == "Ed25519":
        return _bytes32(value)
    if algorithm != "P256-SHA256" or not isinstance(value, bytes) or (
            not 60 <= len(value) <= 160):
        raise EnrollmentError("unsupported public-key algorithm or encoding")
    try:
        pub = serialization.load_der_public_key(value)
    except (ValueError, TypeError) as exc:
        raise EnrollmentError("invalid P256 SPKI") from exc
    if not isinstance(pub, ec.EllipticCurvePublicKey) or (
            not isinstance(pub.curve, ec.SECP256R1)):
        raise EnrollmentError("P256 curve required")
    if pub.public_bytes(serialization.Encoding.DER,
                        serialization.PublicFormat.SubjectPublicKeyInfo) != value:
        raise EnrollmentError("noncanonical P256 SPKI")
    return value


@dataclass(frozen=True)
class Pending:
    request_id: str
    challenge: bytes
    fingerprint: str
    expires_at: int


def signed_challenge(pending: Pending, installation_id: str, public_key: bytes,
                     *, algorithm: str = "Ed25519") -> bytes:
    """Algorithm-domain-separated, non-replayable phone enrollment proof."""
    _id(installation_id)
    _validated_public_key(public_key, algorithm)
    if not isinstance(pending, Pending) or not re.fullmatch(r"[0-9a-f]{32}", pending.request_id):
        raise EnrollmentError("invalid pending request")
    if not isinstance(pending.challenge, bytes) or len(pending.challenge) != 32:
        raise EnrollmentError("invalid challenge")
    domain = CONTEXT if algorithm == "Ed25519" else P256_CONTEXT
    return (domain + bytes.fromhex(pending.request_id) + pending.challenge +
            public_key + installation_id.encode("ascii"))


class EdgeEnrollment:
    def __init__(self, path: str | Path):
        self.path = Path(path)
        if not self.path.parent.is_dir():
            raise EnrollmentError("database parent missing")
        if not self.path.exists():
            fd = os.open(str(self.path), os.O_CREAT | os.O_EXCL | os.O_RDWR, 0o600)
            os.close(fd)
        if stat.S_IMODE(self.path.stat().st_mode) & 0o077:
            raise EnrollmentError("enrollment database must not be group/world-accessible")
        with self._session() as db:
            db.execute("PRAGMA journal_mode=WAL")
            db.executescript("""
                CREATE TABLE IF NOT EXISTS edge_pending (
                    request_id TEXT PRIMARY KEY,
                    installation_id TEXT NOT NULL UNIQUE,
                    public_key BLOB NOT NULL,
                    challenge BLOB NOT NULL,
                    expires_at INTEGER NOT NULL,
                    owner_code_hash BLOB,
                    site_id TEXT,
                    actor_id TEXT,
                    scopes_json TEXT);
                CREATE TABLE IF NOT EXISTS edge_installs (
                    installation_id TEXT PRIMARY KEY,
                    public_key BLOB NOT NULL,
                    site_id TEXT NOT NULL,
                    actor_id TEXT NOT NULL,
                    scopes_json TEXT NOT NULL,
                    token_hash BLOB NOT NULL UNIQUE,
                    expires_at INTEGER NOT NULL,
                    revoked_at INTEGER);
                CREATE INDEX IF NOT EXISTS edge_token_live
                    ON edge_installs(token_hash, revoked_at);
            """)
            # Additive schema migration. Existing read-only PC Ed25519
            # tokens remain valid; no token/key is rewritten.
            for table in ("edge_pending", "edge_installs"):
                fields = {r[1] for r in db.execute(f"PRAGMA table_info({table})")}
                if "key_algorithm" not in fields:
                    db.execute(f"ALTER TABLE {table} ADD COLUMN key_algorithm "
                               "TEXT NOT NULL DEFAULT 'Ed25519'")

    @contextmanager
    def _session(self):
        db = sqlite3.connect(str(self.path), timeout=8)
        try:
            db.row_factory = sqlite3.Row
            db.execute("PRAGMA busy_timeout=8000")
            db.execute("PRAGMA synchronous=FULL")
            yield db
            db.commit()
        except BaseException:
            db.rollback()
            raise
        finally:
            db.close()

    @staticmethod
    def fingerprint(public_key: bytes, *, algorithm: str = "Ed25519") -> str:
        return hashlib.sha256(_validated_public_key(
            public_key, algorithm)).hexdigest().upper()[:20]

    def begin(self, installation_id: str, public_key: bytes,
              *, algorithm: str = "Ed25519") -> Pending:
        """Untrusted request, bounded globally to avoid persistent DB exhaustion."""
        _id(installation_id)
        _validated_public_key(public_key, algorithm)
        now = int(time.time())
        req = secrets.token_hex(16)
        challenge = secrets.token_bytes(32)
        expires = now + MAX_PENDING_SECONDS
        with self._session() as db:
            db.execute("BEGIN IMMEDIATE")
            active = db.execute(
                "SELECT 1 FROM edge_installs WHERE installation_id=?",
                (installation_id,)).fetchone()
            if active:
                raise EnrollmentRefused("installation already enrolled; revocation is separate")
            # Delete only expired pending requests. Live owner-approved
            # registrations cannot be silently replaced by anonymous begin.
            db.execute("DELETE FROM edge_pending WHERE expires_at<=?", (now,))
            existing = db.execute(
                "SELECT owner_code_hash FROM edge_pending WHERE installation_id=?",
                (installation_id,)).fetchone()
            if existing and existing["owner_code_hash"] is not None:
                raise EnrollmentRefused("owner-approved request cannot be replaced")
            if not existing and db.execute(
                    "SELECT COUNT(*) FROM edge_pending").fetchone()[0] >= MAX_PENDING_REQUESTS:
                raise EnrollmentRefused("pending registration capacity reached")
            # Before owner approval, repeat begin invalidates the old nonce.
            db.execute("DELETE FROM edge_pending WHERE installation_id=?",
                       (installation_id,))
            db.execute("INSERT INTO edge_pending "
                       "(request_id,installation_id,public_key,challenge,expires_at,key_algorithm) "
                       "VALUES(?,?,?,?,?,?)",
                       (req, installation_id, public_key, challenge, expires,
                        algorithm))
        return Pending(req, challenge, self.fingerprint(
            public_key, algorithm=algorithm), expires)

    def approve(self, request_id: str, expected_fingerprint: str,
                site_id: str, actor_id: str, scopes: FrozenSet[str]) -> str:
        """TRUSTED OWNER OPERATOR ONLY. Returns a fresh one-time secret.

        The operator must compare the phone's independently shown public-key
        fingerprint and request ID before approving. This method MUST NEVER be
        reachable via anonymous or shared bench-token HTTP.
        """
        if not isinstance(request_id, str) or not re.fullmatch(r"[0-9a-f]{32}", request_id):
            raise EnrollmentError("invalid request")
        _id(site_id)
        _id(actor_id)
        if not isinstance(expected_fingerprint, str) or not re.fullmatch(
            r"[0-9A-F]{20}", expected_fingerprint
        ):
            raise EnrollmentError("invalid fingerprint")
        if not isinstance(scopes, frozenset) or not scopes or not scopes <= SCOPES:
            raise EnrollmentError("invalid scopes")
        code = secrets.token_urlsafe(32)
        now = int(time.time())
        with self._session() as db:
            db.execute("BEGIN IMMEDIATE")
            row = db.execute("SELECT public_key,expires_at,key_algorithm FROM edge_pending "
                             "WHERE request_id=?", (request_id,)).fetchone()
            if not row or int(row["expires_at"]) <= now:
                raise EnrollmentRefused("pending request missing or expired")
            if not hmac.compare_digest(self.fingerprint(
                    row["public_key"], algorithm=row["key_algorithm"]),
                    expected_fingerprint):
                raise EnrollmentRefused("phone key fingerprint mismatch")
            db.execute("UPDATE edge_pending SET owner_code_hash=?,site_id=?,"
                       "actor_id=?,scopes_json=? WHERE request_id=?",
                       (hashlib.sha256(code.encode()).digest(), site_id, actor_id,
                        json.dumps(sorted(scopes)), request_id))
        return code

    def finish(self, request_id: str, installation_id: str,
               public_key: bytes, signature: bytes, owner_code: str,
               *, algorithm: str = "Ed25519") -> str:
        """Bind approved installation to its private key; issue token ONCE.

        Server stores only SHA256(token), never the bearer credential itself.
        Caller delivers it over a protected channel and securely stores it on
        the phone. No upload/command/Tracker permission is implied by enrollment.
        """
        _id(installation_id)
        _validated_public_key(public_key, algorithm)
        if not isinstance(signature, bytes) or (
                len(signature) != 64 if algorithm == "Ed25519"
                else not 8 <= len(signature) <= 80):
            raise EnrollmentRefused("signature missing")
        if not isinstance(owner_code, str) or not 30 <= len(owner_code) <= 80:
            raise EnrollmentRefused("owner approval missing")
        if not isinstance(request_id, str) or not re.fullmatch(r"[0-9a-f]{32}", request_id):
            raise EnrollmentRefused("invalid request")
        now = int(time.time())
        with self._session() as db:
            db.execute("BEGIN IMMEDIATE")
            row = db.execute("SELECT * FROM edge_pending WHERE request_id=?",
                             (request_id,)).fetchone()
            if not row or int(row["expires_at"]) <= now or row["site_id"] is None:
                raise EnrollmentRefused("not approved or expired")
            if (row["installation_id"] != installation_id or
                    row["key_algorithm"] != algorithm or not hmac.compare_digest(
                        row["public_key"], public_key)):
                raise EnrollmentRefused("installation key mismatch")
            if not hmac.compare_digest(
                row["owner_code_hash"], hashlib.sha256(owner_code.encode()).digest()
            ):
                raise EnrollmentRefused("owner approval mismatch")
            p = Pending(request_id, row["challenge"],
                        self.fingerprint(public_key, algorithm=algorithm),
                        int(row["expires_at"]))
            try:
                proof = signed_challenge(p, installation_id, public_key,
                                         algorithm=algorithm)
                if algorithm == "Ed25519":
                    Ed25519PublicKey.from_public_bytes(public_key).verify(signature, proof)
                else:
                    key = serialization.load_der_public_key(public_key)
                    key.verify(signature, proof, ec.ECDSA(hashes.SHA256()))
            except (InvalidSignature, ValueError) as exc:
                raise EnrollmentRefused("phone proof rejected") from exc
            if db.execute("SELECT 1 FROM edge_installs WHERE installation_id=?",
                          (installation_id,)).fetchone():
                raise EnrollmentRefused("already enrolled")
            token = secrets.token_urlsafe(48)
            db.execute("INSERT INTO edge_installs "
                       "(installation_id,public_key,site_id,actor_id,scopes_json,"
                       "token_hash,expires_at,key_algorithm) VALUES (?,?,?,?,?,?,?,?)",
                       (installation_id, public_key, row["site_id"], row["actor_id"],
                        row["scopes_json"], hashlib.sha256(token.encode()).digest(),
                        now + MAX_SESSION_SECONDS, algorithm))
            db.execute("DELETE FROM edge_pending WHERE request_id=?", (request_id,))
        return token

    def resolve_token(self, bearer: str) -> Actor | None:
        if not isinstance(bearer, str) or not 48 <= len(bearer) <= 128:
            return None
        # No caller-controlled actor/site/permissions enter this path.
        now = int(time.time())
        digest = hashlib.sha256(bearer.encode()).digest()
        with self._session() as db:
            row = db.execute(
                "SELECT site_id,actor_id,installation_id,scopes_json "
                "FROM edge_installs WHERE token_hash=? AND revoked_at IS NULL "
                "AND expires_at>?", (digest, now)).fetchone()
        if not row:
            return None
        return Actor(row["site_id"], row["actor_id"], row["installation_id"],
                     frozenset(json.loads(row["scopes_json"])))

    def resolve_headers(self, headers: object) -> Actor | None:
        auth = headers.get("Authorization", "") if hasattr(headers, "get") else ""
        if not isinstance(auth, str) or not auth.startswith("Bearer "):
            return None
        return self.resolve_token(auth[7:])

    def revoke(self, installation_id: str) -> bool:
        """TRUSTED OWNER OPERATOR ONLY. No end-user/public HTTP route here."""
        _id(installation_id)
        with self._session() as db:
            db.execute("BEGIN IMMEDIATE")
            row = db.execute("UPDATE edge_installs SET revoked_at=? WHERE "
                             "installation_id=? AND revoked_at IS NULL",
                             (int(time.time()), installation_id))
            return row.rowcount == 1
