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

Step 3B adds the Gateway issuer: one Ed25519 key in the same registry. Its
public key is pinned into the Gateway firmware at build time; the private key
signs each Gateway's OGK1 grant snapshot (its Trackers' scoped custody ACK
keys), which the Gateway stores in its security region after checking the
signature, its own id and a higher snapshot generation.

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
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
from cryptography.hazmat.primitives.kdf.hkdf import HKDF
from cryptography.hazmat.primitives.serialization import (Encoding, NoEncryption,
                                                          PrivateFormat, PublicFormat)

ACK_INFO_LABEL = b"ORUN-TLP-V2-GW-CUSTODY-ACK-v1"   # custody_ack_verifier.cpp
AEAD_INFO_LABEL = b"ORUN-TLP-V2-AEAD"               # SF5B ROOT_AEAD_V1, D2A
TEST_EPOCH = 0x01020304                              # public bench credential

# OGK1 Gateway grant snapshot (gateway_grant_snapshot_reader.h, SF5K3Y).
OGK1_MAGIC = 0x4F474B31
OGK1_VERSION = 1
OGK1_SLOTS = 32
OGK1_HEADER = 32
OGK1_SLOT = 56
OGK1_PAYLOAD = OGK1_HEADER + OGK1_SLOT * OGK1_SLOTS   # 1824, signed
OGK1_PROOF = 64                                        # Ed25519 signature
OGK1_CRC_OFFSET = OGK1_PAYLOAD + OGK1_PROOF            # 1888
OGK1_COMMIT_OFFSET = OGK1_CRC_OFFSET + 4               # 1892
OGK1_STORED = OGK1_COMMIT_OFFSET + 4                   # 1896
OGK1_ISSUER_KEY_NUMBER = 1     # header offset 24: which issuer key signed it
OGK1_DATA_CHUNK = 64           # bytes per "BENCH OGK1 DATA" line


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


def build_ogk1(gateway_id: int, generation: int,
               grants: list[tuple[Credential, int, int]],
               issuer: Ed25519PrivateKey) -> bytes:
    """Signed OGK1 snapshot image (the 1896 stored bytes, commit word 0).

    grants: (credential, custody_floor, grant_generation) per Tracker; each
    slot holds that Tracker's scoped custody ACK key for this Gateway.
    """
    if not 0 < gateway_id < 2**64 - 1:
        raise RegistryError("gateway id")
    if not 0 < generation < 2**64 - 1:
        raise RegistryError("snapshot generation")
    if len(grants) > OGK1_SLOTS:
        raise RegistryError(f"at most {OGK1_SLOTS} Trackers per Gateway")
    seen: set[int] = set()
    slots = b""
    for cred, floor, grant_gen in sorted(grants, key=lambda g: g[0].device_id):
        dev = cred.device_id
        if dev in seen or dev == gateway_id or not 0 < dev < 2**64 - 1:
            raise RegistryError(f"tracker {dev:016X}")
        if not (cred.key_epoch < 0xFFFFFFFF and floor < 0xFFFFFFFF and
                0 < grant_gen < 0xFFFFFFFF):
            raise RegistryError(f"grant scope {dev:016X}")
        seen.add(dev)
        key = custody_ack_key(cred, gateway_id, floor, grant_gen)
        slots += (dev.to_bytes(8, "big") + cred.key_epoch.to_bytes(4, "big") +
                  floor.to_bytes(4, "big") + grant_gen.to_bytes(4, "big") +
                  (1).to_bytes(4, "big") + key)
    slots += bytes(OGK1_SLOT * (OGK1_SLOTS - len(seen)))
    header = (OGK1_MAGIC.to_bytes(4, "big") + bytes((OGK1_VERSION, OGK1_SLOTS, 0, 0)) +
              gateway_id.to_bytes(8, "big") + generation.to_bytes(8, "big") +
              OGK1_ISSUER_KEY_NUMBER.to_bytes(4, "big") + bytes(4))
    payload = header + slots
    assert len(payload) == OGK1_PAYLOAD
    signed = payload + issuer.sign(payload)
    image = signed + (zlib.crc32(signed) & 0xFFFFFFFF).to_bytes(4, "big") + bytes(4)
    assert len(image) == OGK1_STORED
    return image


def ogk1_upload_lines(image: bytes) -> list[str]:
    """USB lines for the Gateway bench image (CRC-32 = typo guard only)."""
    if len(image) != OGK1_STORED:
        raise RegistryError("OGK1 image size")
    lines = ["BENCH OGK1 BEGIN"]
    for off in range(0, len(image), OGK1_DATA_CHUNK):
        lines.append(f"BENCH OGK1 DATA {off:04X} {image[off:off + OGK1_DATA_CHUNK].hex().upper()}")
    lines.append(f"BENCH OGK1 COMMIT {zlib.crc32(image) & 0xFFFFFFFF:08X}")
    return lines


def issuer_public(issuer: Ed25519PrivateKey) -> bytes:
    return issuer.public_key().public_bytes(Encoding.Raw, PublicFormat.Raw)


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
            # Step 3B: the single Gateway issuer key and the last OGK1
            # snapshot generation handed to each Gateway (only ever grows).
            db.execute(
                "CREATE TABLE IF NOT EXISTS issuer ("
                " id INTEGER PRIMARY KEY CHECK(id=1),"
                " private_key TEXT NOT NULL CHECK(length(private_key)=64),"
                " public_key TEXT NOT NULL CHECK(length(public_key)=64),"
                " created_utc INTEGER NOT NULL)")
            db.execute(
                "CREATE TABLE IF NOT EXISTS gateway_snapshots ("
                " gateway_id TEXT PRIMARY KEY CHECK(length(gateway_id)=16),"
                " generation INTEGER NOT NULL CHECK(generation>0),"
                " issued_utc INTEGER NOT NULL)")

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

    # ---- Step 3B: Gateway issuer and signed grant snapshots ----

    def issuer(self, create: bool = False) -> Optional[Ed25519PrivateKey]:
        """The Gateway issuer key (created once on request, never replaced)."""
        with closing(self._conn()) as db:
            r = db.execute("SELECT private_key FROM issuer WHERE id=1").fetchone()
            if r is None and create:
                key = Ed25519PrivateKey.generate()
                # private_bytes(Raw) works on every cryptography release with
                # Ed25519 (private_bytes_raw() needs >= 40).
                seed = key.private_bytes(Encoding.Raw, PrivateFormat.Raw, NoEncryption())
                db.execute("INSERT OR IGNORE INTO issuer VALUES (1,?,?,?)",
                           (seed.hex().upper(), issuer_public(key).hex().upper(),
                            int(time.time())))
                r = db.execute("SELECT private_key FROM issuer WHERE id=1").fetchone()
        return Ed25519PrivateKey.from_private_bytes(bytes.fromhex(r[0])) if r else None

    def next_snapshot_generation(self, gateway_id: int, at_least: int = 0) -> int:
        """Reserve the next OGK1 generation for a Gateway (never reused).

        at_least: the generation the Gateway reports as active, so a lost
        registry row can never make the Gateway refuse (or roll back).
        """
        gw = _hex16(gateway_id)
        with closing(self._conn()) as db:
            db.execute("BEGIN IMMEDIATE")
            try:
                r = db.execute("SELECT generation FROM gateway_snapshots WHERE gateway_id=?",
                               (gw,)).fetchone()
                generation = max(r[0] if r else 0, at_least) + 1
                if generation >= 2**63:
                    raise RegistryError("snapshot generations exhausted")
                db.execute("INSERT OR REPLACE INTO gateway_snapshots VALUES (?,?,?)",
                           (gw, generation, int(time.time())))
                db.execute("COMMIT")
            except Exception:
                db.execute("ROLLBACK")
                raise
        return generation

    def gateway_snapshot(self, gateway_id: int, at_least: int = 0) -> tuple[int, bytes]:
        """(generation, signed OGK1 image) with every active grant of this Gateway."""
        issuer = self.issuer()
        if issuer is None:
            raise RegistryError("no Gateway issuer key yet (run: orun_provision.py issuer)")
        grants = self.grants_for_gateway(gateway_id)
        generation = self.next_snapshot_generation(gateway_id, at_least)
        return generation, build_ogk1(gateway_id, generation, grants, issuer)
