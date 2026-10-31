"""ORUN account sessions: username/password on any authorized HTTPS client.

Account login is NOT tied to Android P256 phone enrollment. Password sessions
are hashed, expire after 12 hours, and resolve server-side owner permissions.
The old P256-enrolled login continues to work for already installed APKs.
No user/site/actor/scopes are trusted from login request metadata.
PC Ed25519 mirror authorization is independent and unchanged.
This dev tailnet implementation is not a public-internet abuse audit.
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

from .edge_enrollment import EdgeEnrollment
from .shared_sync_store import Actor

USERNAME = re.compile(r"^[a-z][a-z0-9._-]{2,63}$")
ID = re.compile(r"^[A-Za-z0-9._-]{1,128}$")
OWNER_SCOPES = frozenset({
    "zone:read", "zone:write", "group:read", "group:write",
    "zone_target:read", "zone_target:write",
})
MAX_LOGIN_FAILURES = 5
LOCK_SECONDS = 15 * 60
SESSION_SECONDS = 12 * 60 * 60
SCRYPT_N = 1 << 15
SCRYPT_R = 8
SCRYPT_P = 1
MAX_SCRYPT_MEMORY = 64 * 1024 * 1024


class AccountError(ValueError):
    pass


class LoginRejected(AccountError):
    pass


class LoginLocked(LoginRejected):
    pass


@dataclass(frozen=True)
class IssuedAccountSession:
    bearer: str
    site: str
    actor: str
    username: str
    installation: str
    expires_at: int


def _safe_id(value: str) -> str:
    if not isinstance(value, str) or not ID.fullmatch(value):
        raise AccountError("invalid identifier")
    return value


def _username(value: str) -> str:
    if not isinstance(value, str) or not USERNAME.fullmatch(value):
        raise AccountError("invalid username")
    return value


def _password(value: str) -> str:
    if (not isinstance(value, str) or not 12 <= len(value) <= 128 or
            "\x00" in value):
        raise AccountError("password must be between 12 and 128 characters")
    return value


def _hash_password(value: str, salt: bytes) -> bytes:
    return hashlib.scrypt(value.encode("utf-8"), salt=salt, n=SCRYPT_N,
                          r=SCRYPT_R, p=SCRYPT_P, dklen=32,
                          maxmem=MAX_SCRYPT_MEMORY)


class AccountStore:
    def __init__(self, path: str | Path):
        self.path = Path(path)
        if not self.path.parent.is_dir():
            raise AccountError("account database parent missing")
        if self.path.is_symlink():
            raise AccountError("account database symlink denied")
        if not self.path.exists():
            fd = os.open(str(self.path), os.O_CREAT | os.O_EXCL | os.O_RDWR, 0o600)
            os.close(fd)
        info = self.path.stat()
        if (not stat.S_ISREG(info.st_mode) or info.st_uid != os.geteuid() or
                stat.S_IMODE(info.st_mode) & 0o077):
            raise AccountError("account database permissions must be private")
        with self._session() as db:
            db.execute("PRAGMA journal_mode=WAL")
            db.executescript("""
                CREATE TABLE IF NOT EXISTS account_users (
                    site_id TEXT NOT NULL,
                    actor_id TEXT NOT NULL,
                    username TEXT NOT NULL,
                    salt BLOB NOT NULL,
                    verifier BLOB NOT NULL,
                    password_version INTEGER NOT NULL DEFAULT 1,
                    disabled INTEGER NOT NULL DEFAULT 0 CHECK(disabled IN (0,1)),
                    PRIMARY KEY(site_id, actor_id),
                    UNIQUE(site_id, username));
                CREATE TABLE IF NOT EXISTS account_attempts (
                    site_id TEXT NOT NULL,
                    installation_id TEXT NOT NULL,
                    failed_count INTEGER NOT NULL,
                    locked_until INTEGER NOT NULL,
                    PRIMARY KEY(site_id, installation_id));
                CREATE TABLE IF NOT EXISTS account_sessions (
                    token_hash BLOB PRIMARY KEY,
                    site_id TEXT NOT NULL,
                    actor_id TEXT NOT NULL,
                    installation_id TEXT NOT NULL,
                    device_token_hash BLOB NOT NULL,
                    password_version INTEGER NOT NULL,
                    issued_at INTEGER NOT NULL,
                    expires_at INTEGER NOT NULL,
                    revoked_at INTEGER);
                CREATE INDEX IF NOT EXISTS account_sessions_installation
                  ON account_sessions(installation_id, revoked_at);
                CREATE TABLE IF NOT EXISTS account_password_attempts (
                    username TEXT PRIMARY KEY,
                    failed_count INTEGER NOT NULL,
                    locked_until INTEGER NOT NULL);
                CREATE TABLE IF NOT EXISTS account_password_sessions (
                    token_hash BLOB PRIMARY KEY,
                    site_id TEXT NOT NULL,
                    actor_id TEXT NOT NULL,
                    installation_id TEXT NOT NULL,
                    password_version INTEGER NOT NULL,
                    issued_at INTEGER NOT NULL,
                    expires_at INTEGER NOT NULL,
                    revoked_at INTEGER);
                CREATE INDEX IF NOT EXISTS account_password_sessions_actor
                  ON account_password_sessions(site_id, actor_id, revoked_at);
            """)

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

    def bootstrap_owner(self, site: str, actor: str,
                        username: str, password: str) -> None:
        """Trusted local CLI ONLY. Will not overwrite an existing site owner.

        The actor name must be the actor approved for the phone enrollment.
        No default username/password, registration token or API bootstrap.
        """
        _safe_id(site)
        _safe_id(actor)
        _username(username)
        _password(password)
        salt = secrets.token_bytes(16)
        verifier = _hash_password(password, salt)
        with self._session() as db:
            db.execute("BEGIN IMMEDIATE")
            if db.execute("SELECT 1 FROM account_users WHERE site_id=? LIMIT 1",
                          (site,)).fetchone():
                raise AccountError("account site already initialized")
            db.execute(
                "INSERT INTO account_users "
                "(site_id,actor_id,username,salt,verifier) VALUES(?,?,?,?,?)",
                (site, actor, username, salt, verifier))

    def password_login(self, username: str, password: str) -> IssuedAccountSession:
        """Password-only login; no pairing, fingerprint or phone grant.

        The username must resolve to exactly ONE account across hosted sites.
        Tailscale private HTTP boundary and HTTPS are deployment requirements.
        Username-global persistent failed-attempt lockout protects the scrypt
        work factor from repeated password guessing.
        """
        if (not isinstance(username, str) or len(username) > 64 or
                not isinstance(password, str) or len(password) > 128):
            raise LoginRejected("invalid credentials")
        now = int(time.time())
        with self._session() as db:
            db.execute("BEGIN IMMEDIATE")
            throttle = db.execute(
                "SELECT failed_count, locked_until FROM account_password_attempts "
                "WHERE username=?", (username,)).fetchone()
            if throttle and int(throttle["locked_until"]) > now:
                raise LoginLocked("login temporarily restricted")
            candidates = db.execute(
                "SELECT site_id,actor_id,username,salt,verifier,password_version,disabled "
                "FROM account_users WHERE username=? LIMIT 2", (username,)).fetchall()
            row = candidates[0] if len(candidates) == 1 else None
            salt = row["salt"] if row else b"ORUN-LOGIN-DUMMY!"
            verifier = row["verifier"] if row else bytes(32)
            calculated = _hash_password(password, salt)
            valid = bool(row and not row["disabled"] and
                         USERNAME.fullmatch(username) and
                         hmac.compare_digest(calculated, verifier))
            if not valid:
                prior = (int(throttle["failed_count"]) if throttle and
                         not (throttle["locked_until"] and
                              int(throttle["locked_until"]) <= now) else 0)
                failures = prior + 1
                locked = now + LOCK_SECONDS if failures >= MAX_LOGIN_FAILURES else 0
                db.execute(
                    "INSERT INTO account_password_attempts VALUES(?,?,?) "
                    "ON CONFLICT(username) DO UPDATE SET "
                    "failed_count=excluded.failed_count,"
                    "locked_until=excluded.locked_until",
                    (username, failures, locked))
                db.commit()
                if locked:
                    raise LoginLocked("login temporarily restricted")
                raise LoginRejected("invalid credentials")
            db.execute("DELETE FROM account_password_attempts WHERE username=?",
                       (username,))
            site = str(row["site_id"])
            actor = str(row["actor_id"])
            # Stable account namespace allows the same local SQLite mirror to
            # be reused after logout/relogin; it is NOT an authenticator.
            account_installation = "account-" + hashlib.sha256(
                (site + "\\0" + actor).encode("utf-8")).hexdigest()
            token = secrets.token_urlsafe(48)
            db.execute(
                "INSERT INTO account_password_sessions "
                "(token_hash,site_id,actor_id,installation_id,"
                "password_version,issued_at,expires_at) VALUES(?,?,?,?,?,?,?)",
                (hashlib.sha256(token.encode("ascii")).digest(), site,
                 actor, account_installation, int(row["password_version"]),
                 now, now + SESSION_SECONDS))
            return IssuedAccountSession(
                token, site, actor, username, account_installation,
                now + SESSION_SECONDS)

    def resolve_password_session(self, bearer: str) -> Actor | None:
        """Resolve user authority from current DB, never from a client claim."""
        if (not isinstance(bearer, str) or not
                re.fullmatch(r"[A-Za-z0-9_-]{48,128}", bearer)):
            return None
        now = int(time.time())
        with self._session() as db:
            row = db.execute(
                "SELECT s.site_id,s.actor_id,s.installation_id,"
                "s.password_version,u.disabled,"
                "u.password_version AS current_version "
                "FROM account_password_sessions s JOIN account_users u "
                "ON u.site_id=s.site_id AND u.actor_id=s.actor_id "
                "WHERE s.token_hash=? AND s.revoked_at IS NULL "
                "AND s.expires_at>?",
                (hashlib.sha256(bearer.encode("ascii")).digest(), now),
            ).fetchone()
        if (not row or row["disabled"] or
                row["password_version"] != row["current_version"]):
            return None
        return Actor(row["site_id"], row["actor_id"],
                     row["installation_id"], OWNER_SCOPES)

    def login(self, enrollment: EdgeEnrollment, device_bearer: str,
              username: str, password: str) -> IssuedAccountSession:
        """Issue a short-lived account bearer ONLY over a current P256 phone.

        Credential failures are deliberately indistinguishable to clients.
        Per enrolled installation persistent rate limit makes guessing bounded
        without unauthenticated user enumeration.
        """
        pair = enrollment.resolve_token_with_algorithm(device_bearer)
        if not pair or pair[1] != "P256-SHA256":
            raise LoginRejected("invalid credentials")
        device = pair[0]
        if not isinstance(username, str) or len(username) > 64 or (
                not isinstance(password, str) or len(password) > 128):
            raise LoginRejected("invalid credentials")
        now = int(time.time())
        with self._session() as db:
            db.execute("BEGIN IMMEDIATE")
            throttle = db.execute(
                "SELECT failed_count,locked_until FROM account_attempts "
                "WHERE site_id=? AND installation_id=?",
                (device.site_id, device.installation_id)).fetchone()
            if throttle and int(throttle["locked_until"]) > now:
                raise LoginLocked("login temporarily restricted")
            row = db.execute(
                "SELECT username,salt,verifier,password_version,disabled "
                "FROM account_users WHERE site_id=? AND actor_id=?",
                (device.site_id, device.actor_id)).fetchone()
            # Unknown user, wrong actor and wrong password take the same
            # relatively expensive password-verifier path.
            salt = row["salt"] if row else b"ORUN-S6A-DUMMY!!"
            correct = row["verifier"] if row else bytes(32)
            # Bound the workload even for malformed or enormous input.
            supplied = password if isinstance(password, str) and (
                len(password) <= 128) else ""
            calculated = _hash_password(supplied, salt)
            good = bool(row and not row["disabled"] and
                        USERNAME.fullmatch(username) and
                        hmac.compare_digest(row["username"], username) and
                        hmac.compare_digest(calculated, correct))
            if not good:
                prior = (int(throttle["failed_count"]) if throttle and
                         not (throttle["locked_until"] and
                              int(throttle["locked_until"]) <= now) else 0)
                failures = prior + 1
                locked = now + LOCK_SECONDS if failures >= MAX_LOGIN_FAILURES else 0
                db.execute(
                    "INSERT INTO account_attempts VALUES(?,?,?,?) "
                    "ON CONFLICT(site_id,installation_id) DO UPDATE SET "
                    "failed_count=excluded.failed_count,"
                    "locked_until=excluded.locked_until",
                    (device.site_id, device.installation_id, failures, locked))
                # Commit the failure before raising (the _session context
                # deliberately rolls back on exceptions).
                db.commit()
                if locked:
                    raise LoginLocked("login temporarily restricted")
                raise LoginRejected("invalid credentials")
            db.execute(
                "DELETE FROM account_attempts WHERE site_id=? AND installation_id=?",
                (device.site_id, device.installation_id))
            token = secrets.token_urlsafe(48)
            db.execute(
                "INSERT INTO account_sessions "
                "(token_hash,site_id,actor_id,installation_id,device_token_hash,"
                "password_version,issued_at,expires_at) VALUES(?,?,?,?,?,?,?,?)",
                (hashlib.sha256(token.encode("ascii")).digest(),
                 device.site_id, device.actor_id, device.installation_id,
                 hashlib.sha256(device_bearer.encode("ascii")).digest(),
                 int(row["password_version"]), now, now + SESSION_SECONDS))
            return IssuedAccountSession(
                token, device.site_id, device.actor_id, row["username"],
                device.installation_id, now + SESSION_SECONDS)

    def resolve_session(self, enrollment: EdgeEnrollment,
                        bearer: str) -> Actor | None:
        password_actor = self.resolve_password_session(bearer)
        if password_actor is not None:
            return password_actor
        if (not isinstance(bearer, str) or not
                re.fullmatch(r"[A-Za-z0-9_-]{48,128}", bearer)):
            return None
        now = int(time.time())
        digest = hashlib.sha256(bearer.encode("ascii")).digest()
        with self._session() as db:
            row = db.execute(
                "SELECT s.site_id,s.actor_id,s.installation_id,"
                "s.device_token_hash,s.password_version,u.username,"
                "u.disabled,u.password_version AS current_version "
                "FROM account_sessions s JOIN account_users u "
                "ON u.site_id=s.site_id AND u.actor_id=s.actor_id "
                "WHERE s.token_hash=? AND s.revoked_at IS NULL "
                "AND s.expires_at>?", (digest, now)).fetchone()
        if (not row or row["disabled"] or
                row["password_version"] != row["current_version"]):
            return None
        verified = enrollment.active_installation(
            row["installation_id"], row["device_token_hash"])
        if not verified or verified[1] != "P256-SHA256":
            return None
        device = verified[0]
        if device.site_id != row["site_id"] or device.actor_id != row["actor_id"]:
            return None
        scopes = device.scopes & OWNER_SCOPES
        if not scopes:
            return None
        return Actor(device.site_id, device.actor_id,
                     device.installation_id, frozenset(scopes))

    def revoke_session(self, bearer: str) -> bool:
        if (not isinstance(bearer, str) or not
                re.fullmatch(r"[A-Za-z0-9_-]{48,128}", bearer)):
            return False
        with self._session() as db:
            db.execute("BEGIN IMMEDIATE")
            changed = db.execute(
                "UPDATE account_sessions SET revoked_at=? "
                "WHERE token_hash=? AND revoked_at IS NULL",
                (int(time.time()),
                 hashlib.sha256(bearer.encode("ascii")).digest()))
            password_changed = db.execute(
                "UPDATE account_password_sessions SET revoked_at=? "
                "WHERE token_hash=? AND revoked_at IS NULL",
                (int(time.time()),
                 hashlib.sha256(bearer.encode("ascii")).digest()))
        return changed.rowcount == 1 or password_changed.rowcount == 1
