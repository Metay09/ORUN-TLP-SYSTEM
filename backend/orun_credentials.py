"""Step 3A: backend credential registry (real per-Tracker keys).

The backend is the only place a Tracker's root key lives after provisioning
(ADR M7P6 §3, §11). It hands out ONLY derived, scoped material:

- to the Tracker, once over USB at provisioning (physical access): its own
  credential (credential_id, key_epoch, K_root);
- to a Gateway, over USB: the scoped custody ACK key for one
  (Tracker, Gateway, epoch, floor, generation) — never K_root;
- to the owner's phone (owner decision D1): the per-epoch D2A traffic key,
  so the phone can read its own Trackers offline — never K_root.

Rules: credentials are random (secrets), never logged; a device's epochs
only grow and are never reused, so (device, epoch) names exactly one
credential lifetime; retired credentials are kept read-only so older records
still open; a credential_id or root is never reintroduced. The registry file
is created owner-read/write only (0600).

BENCH note: storage is a plain SQLite file on the owner's machine; key
wrapping/escrow and hardware protection are later work.
"""
from __future__ import annotations

from contextlib import closing
from dataclasses import dataclass
import os
from pathlib import Path
import secrets
import sqlite3
import time
from typing import Optional
import zlib

from cryptography.hazmat.primitives import hashes
from cryptography.hazmat.primitives.kdf.hkdf import HKDF

ACK_INFO_LABEL = b"ORUN-TLP-V2-GW-CUSTODY-ACK-v1"   # custody_ack_verifier.cpp
AEAD_INFO_LABEL = b"ORUN-TLP-V2-AEAD"               # SF5B ROOT_AEAD_V1, D2A
TEST_EPOCH = 0x01020304                              # public bench credential


class RegistryError(Exception):
    pass


@dataclass(frozen=True)
class Credential:
    device_id: int
    credential_id: bytes
    key_epoch: int
    k_root: bytes
    retired: bool


def _hex16(v: int) -> str:
    return f"{v:016X}"


def custody_ack_key(cred: Credential, gateway_id: int, custody_floor: int,
                    grant_generation: int) -> bytes:
    """Same HKDF as the Tracker verifier and Sf5l0BenchGrantSource."""
    info = (ACK_INFO_LABEL + cred.device_id.to_bytes(8, "big") +
            cred.key_epoch.to_bytes(4, "big") + gateway_id.to_bytes(8, "big") +
            custody_floor.to_bytes(4, "big") + grant_generation.to_bytes(4, "big"))
    assert len(info) == 57
    return HKDF(hashes.SHA256(), 32, cred.credential_id, info).derive(cred.k_root)


def traffic_key(cred: Credential) -> bytes:
    """Per-epoch D2A AEAD key (what the owner's phone gets; never K_root)."""
    info = AEAD_INFO_LABEL + bytes((1,)) + cred.key_epoch.to_bytes(4, "big")
    return HKDF(hashes.SHA256(), 16, cred.credential_id, info).derive(cred.k_root)


def provision_line(cred: Credential, gateway_id: int) -> str:
    """BENCH PROVISION line for the Tracker (CRC-32 typo guard only)."""
    blob = (cred.credential_id + cred.key_epoch.to_bytes(4, "big") + cred.k_root +
            gateway_id.to_bytes(8, "big"))
    crc = zlib.crc32(blob) & 0xFFFFFFFF
    return (f"BENCH PROVISION {cred.credential_id.hex().upper()} {cred.key_epoch:08X} "
            f"{cred.k_root.hex().upper()} {_hex16(gateway_id)} {crc:08X}")


def grant_line(cred: Credential, gateway_id: int, custody_floor: int,
               grant_generation: int) -> str:
    """BENCH GRANT line for the Gateway (CRC-32 typo guard only)."""
    key = custody_ack_key(cred, gateway_id, custody_floor, grant_generation)
    blob = (cred.device_id.to_bytes(8, "big") + cred.key_epoch.to_bytes(4, "big") +
            custody_floor.to_bytes(4, "big") + grant_generation.to_bytes(4, "big") + key)
    crc = zlib.crc32(blob) & 0xFFFFFFFF
    return (f"BENCH GRANT {_hex16(cred.device_id)} {cred.key_epoch:08X} {custody_floor:08X} "
            f"{grant_generation:08X} {key.hex().upper()} {crc:08X}")


class CredentialRegistry:
    def __init__(self, path: str | Path):
        self.path = Path(path)
        if not self.path.exists():
            # Owner-only from the first byte.
            fd = os.open(str(self.path), os.O_CREAT | os.O_WRONLY | os.O_EXCL, 0o600)
            os.close(fd)
        with closing(self._conn()) as db:
            db.execute(
                "CREATE TABLE IF NOT EXISTS credentials ("
                " device_id TEXT NOT NULL CHECK(length(device_id)=16),"
                " key_epoch INTEGER NOT NULL CHECK(key_epoch>0 AND key_epoch<4294967295),"
                " credential_id TEXT NOT NULL UNIQUE CHECK(length(credential_id)=32),"
                " k_root TEXT NOT NULL UNIQUE CHECK(length(k_root)=64),"
                " created_utc INTEGER NOT NULL, retired INTEGER NOT NULL DEFAULT 0,"
                " PRIMARY KEY (device_id, key_epoch))")
            db.execute(
                "CREATE TABLE IF NOT EXISTS grants ("
                " device_id TEXT NOT NULL, key_epoch INTEGER NOT NULL,"
                " gateway_id TEXT NOT NULL CHECK(length(gateway_id)=16),"
                " custody_floor INTEGER NOT NULL, grant_generation INTEGER NOT NULL,"
                " recorded_utc INTEGER NOT NULL,"
                " PRIMARY KEY (device_id, key_epoch, gateway_id))")

    def _conn(self) -> sqlite3.Connection:
        db = sqlite3.connect(str(self.path), timeout=2.0, isolation_level=None)
        db.execute("PRAGMA synchronous=FULL")
        return db

    @staticmethod
    def _row(r) -> Credential:
        return Credential(int(r[0], 16), bytes.fromhex(r[2]), int(r[1]),
                          bytes.fromhex(r[3]), bool(r[4]))

    def create(self, device_id: int, replace: bool = False) -> Credential:
        """New random credential for a device (next epoch, never reused)."""
        if not 0 < device_id < 2**64:
            raise RegistryError("device id")
        dev = _hex16(device_id)
        with closing(self._conn()) as db:
            db.execute("BEGIN IMMEDIATE")
            try:
                active = db.execute("SELECT 1 FROM credentials WHERE device_id=? AND retired=0",
                                    (dev,)).fetchone()
                if active and not replace:
                    raise RegistryError(f"{dev} already has an active credential (use replace)")
                last = db.execute("SELECT max(key_epoch) FROM credentials WHERE device_id=?",
                                  (dev,)).fetchone()[0] or 0
                epoch = last + 1
                if epoch == TEST_EPOCH:
                    epoch += 1
                if epoch >= 0xFFFFFFFF:
                    raise RegistryError("epochs exhausted")
                db.execute("UPDATE credentials SET retired=1 WHERE device_id=?", (dev,))
                cred_id, root = secrets.token_bytes(16), secrets.token_bytes(32)
                db.execute("INSERT INTO credentials VALUES (?,?,?,?,?,0)",
                           (dev, epoch, cred_id.hex().upper(), root.hex().upper(), int(time.time())))
                db.execute("COMMIT")
            except Exception:
                db.execute("ROLLBACK")
                raise
        return Credential(device_id, cred_id, epoch, root, False)

    def active(self, device_id: int) -> Optional[Credential]:
        with closing(self._conn()) as db:
            r = db.execute("SELECT device_id,key_epoch,credential_id,k_root,retired FROM credentials "
                           "WHERE device_id=? AND retired=0", (_hex16(device_id),)).fetchone()
        return self._row(r) if r else None

    def get(self, device_id: int, key_epoch: int) -> Optional[Credential]:
        with closing(self._conn()) as db:
            r = db.execute("SELECT device_id,key_epoch,credential_id,k_root,retired FROM credentials "
                           "WHERE device_id=? AND key_epoch=?", (_hex16(device_id), key_epoch)).fetchone()
        return self._row(r) if r else None

    def all(self) -> list[Credential]:
        with closing(self._conn()) as db:
            rows = db.execute("SELECT device_id,key_epoch,credential_id,k_root,retired FROM credentials "
                              "ORDER BY device_id,key_epoch").fetchall()
        return [self._row(r) for r in rows]

    def record_grant(self, device_id: int, key_epoch: int, gateway_id: int,
                     custody_floor: int, grant_generation: int) -> None:
        if self.get(device_id, key_epoch) is None:
            raise RegistryError("no such credential")
        with closing(self._conn()) as db:
            db.execute("INSERT OR REPLACE INTO grants VALUES (?,?,?,?,?,?)",
                       (_hex16(device_id), key_epoch, _hex16(gateway_id), custody_floor,
                        grant_generation, int(time.time())))

    def grants_for_gateway(self, gateway_id: int) -> list[tuple[Credential, int, int]]:
        """(credential, floor, generation) for every ACTIVE credential granted to it."""
        with closing(self._conn()) as db:
            rows = db.execute(
                "SELECT g.device_id,g.key_epoch,g.custody_floor,g.grant_generation FROM grants g "
                "JOIN credentials c ON c.device_id=g.device_id AND c.key_epoch=g.key_epoch "
                "WHERE g.gateway_id=? AND c.retired=0 ORDER BY g.device_id",
                (_hex16(gateway_id),)).fetchall()
        return [(self.get(int(d, 16), e), f, g) for d, e, f, g in rows]

    def lookup(self, ledger_module):
        """CredentialLookup for the backend verified ledger: (device, epoch)."""
        def find(device: bytes, epoch: int):
            c = self.get(int.from_bytes(device, "big"), epoch)
            return ledger_module.BackendCredential(c.credential_id, c.k_root) if c else None
        return find
