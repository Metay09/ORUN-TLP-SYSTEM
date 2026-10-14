#!/usr/bin/env python3
"""Real SF5B AEAD + SQLite logical dedup using published independent SF5B KAT.

Test keys are PUBLIC and MUST NEVER be provisioned in deployed ORUN nodes.
Requires python3-cryptography; this is not a simulated/mock crypto pass.
"""
from __future__ import annotations

from contextlib import contextmanager
from pathlib import Path
from threading import Thread
import re
import sqlite3
import sys
import tempfile
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "backend" / "candidates"))
sys.path.insert(0, str(ROOT / "edge" / "candidates"))

from cryptography.hazmat.primitives import hashes
from cryptography.hazmat.primitives.ciphers.aead import AESCCM
from cryptography.hazmat.primitives.kdf.hkdf import HKDF

import product_secure_verified_ledger as secure
import edge_durable_inbox as edge
import loopback_backend_ingress as ingress


def public_fixture(name: str) -> bytes:
    text = (ROOT / "firmware/tests/sf5/sf5b_product_vectors.h").read_text()
    m = re.search(
        r"static const char " + re.escape(name) +
        r"\[\]\s*=\s*((?:\s*\"[0-9a-f]+\"\s*)+);", text,
    )
    assert m is not None, name
    return bytes.fromhex("".join(re.findall(r'\"([0-9a-f]+)\"', m.group(1))))


DEVICE = bytes.fromhex("1122334455667788")
EPOCH = 0x01020304
CRED = secure.BackendCredential(bytes(range(0xA0, 0xB0)), bytes(range(32)))


def keys(device: bytes, epoch: int) -> secure.BackendCredential | None:
    return CRED if device == DEVICE and epoch == EPOCH else None


def reencrypt(original: bytes, counter: int, plain: bytes | None = None) -> bytes:
    item = secure.open_product_frame(original, keys)
    assert item.plaintext
    new_header = bytearray(original[:28])
    new_header[20:28] = counter.to_bytes(8, "big")
    info = b"ORUN-TLP-V2-AEAD" + bytes((1,)) + EPOCH.to_bytes(4, "big")
    key = HKDF(hashes.SHA256(), 16, CRED.credential_id, info).derive(CRED.k_root)
    nonce = EPOCH.to_bytes(4, "big") + bytes((1,)) + counter.to_bytes(8, "big")
    ciphertext = AESCCM(key, tag_length=8).encrypt(
        nonce, item.plaintext if plain is None else plain, bytes(new_header),
    )
    return bytes(new_header) + ciphertext


@contextmanager
def receiver(path: Path):
    server = ingress.LoopbackBackendServer(path)
    thread = Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        yield server
    finally:
        server.shutdown()
        server.server_close()
        thread.join(timeout=3)
        assert not thread.is_alive()


def test_golden_vectors() -> None:
    cases = [
        ("kPeriodicFrameHex", "kPeriodicPlaintextHex", 1, 257),
        ("kEventFrameHex", "kEventPlaintextHex", 2, 258),
        ("kKatPeriodicFrameHex", "kPeriodicPlaintextHex", 1, 257),
        ("kKatEventFrameHex", "kEventPlaintextHex", 2, 258),
    ]
    for raw_name, plain_name, family, seq in cases:
        raw = public_fixture(raw_name)
        obj = secure.open_product_frame(raw, keys)
        assert obj.plaintext == public_fixture(plain_name)
        assert obj.family == family and obj.sequence == seq
        assert obj.device_id == DEVICE and obj.incarnation == bytes.fromhex(
            "a1a2a3a4a5a6a7a8")
    for bad in (b"", b"a" * 84, b"a" * 99):
        try:
            secure.open_product_frame(bad, keys)
        except secure.Reject:
            pass
        else:
            raise AssertionError("accepted fake protected content")


def test_tag_header_key_and_semantic_refusal() -> None:
    original = public_fixture("kPeriodicFrameHex")
    event = public_fixture("kEventFrameHex")
    cases = []
    for index in (0, 1, 2, 3, 4, 5, 6, 7, 8, 16, 20, 28, 55, 99):
        forged = bytearray(original)
        forged[index] ^= 1
        cases.append(bytes(forged))
    cases += [original[:-1], original + b"x"]
    cases += [event[:-1], event + b"x"]
    for raw in cases:
        try:
            secure.open_product_frame(raw, keys)
        except secure.Reject:
            pass
        else:
            raise AssertionError("accepted modified header/tag/ciphertext")
    for resolver in (
        lambda device, epoch: None,
        lambda device, epoch: secure.BackendCredential(
            CRED.credential_id, bytes([5]) * 32),
        lambda device, epoch: secure.BackendCredential(
            bytes([5]) * 16, CRED.k_root),
    ):
        try:
            secure.open_product_frame(original, resolver)
        except secure.Reject:
            pass
        else:
            raise AssertionError("accepted missing or wrong key")

    # Valid AES-CCM tag does not make an invalid application record canonical.
    decrypted = bytearray(secure.open_product_frame(original, keys).plaintext)
    decrypted[20:24] = bytes(4)  # impossible duration=0
    invalid_but_authenticated = reencrypt(original, 100, bytes(decrypted))
    try:
        secure.open_product_frame(invalid_but_authenticated, keys)
    except secure.Reject:
        pass
    else:
        raise AssertionError("accepted authenticated noncanonical plaintext")


def test_ledger_logical_idempotence_and_conflict() -> None:
    with tempfile.TemporaryDirectory() as folder:
        db = Path(folder) / "verified_backend.sqlite"
        ledger = secure.VerifiedLocalLedger(db, keys)
        periodic = public_fixture("kPeriodicFrameHex")
        event = public_fixture("kEventFrameHex")

        assert ledger.ingest(periodic).state == secure.LedgerState.VERIFIED_STORED
        assert ledger.record_count() == 1
        assert ledger.ingest(periodic).state == secure.LedgerState.VERIFIED_DUPLICATE
        # Same logical observation, genuinely different AEAD protected frame:
        # decoded and semantically equivalent plaintext => one DB row.
        assert ledger.ingest(reencrypt(periodic, 101)).state == secure.LedgerState.VERIFIED_DUPLICATE
        assert ledger.ingest(public_fixture("kKatPeriodicFrameHex")).state == secure.LedgerState.VERIFIED_DUPLICATE
        assert ledger.record_count() == 1

        plain = bytearray(secure.open_product_frame(periodic, keys).plaintext)
        plain[56:58] = (4001).to_bytes(2, "big")
        conflicting = reencrypt(periodic, 102, bytes(plain))
        assert ledger.ingest(conflicting).state == secure.LedgerState.CONTENT_CONFLICT
        assert ledger.ingest(conflicting).state == secure.LedgerState.CONTENT_CONFLICT
        assert ledger.record_count() == 1
        assert ledger.conflict_count() == 1  # forensic evidence, no replay multiplication
        with sqlite3.connect(db) as conn:
            row = conn.execute(
                "SELECT exact_first_frame FROM verified_conflicts WHERE sequence=257"
            ).fetchone()
            assert row is not None and bytes(row[0]) == conflicting

        assert ledger.ingest(event).state == secure.LedgerState.VERIFIED_STORED
        assert ledger.record_count() == 2
        cross_family = bytearray(secure.open_product_frame(event, keys).plaintext)
        cross_family[16:20] = (257).to_bytes(4, "big")
        assert ledger.ingest(reencrypt(event, 103, bytes(cross_family))).state == secure.LedgerState.CONTENT_CONFLICT
        assert ledger.record_count() == 2
        assert ledger.conflict_count() == 2

        restarted = secure.VerifiedLocalLedger(db, keys)
        assert restarted.ingest(reencrypt(periodic, 104)).state == secure.LedgerState.VERIFIED_DUPLICATE
        assert restarted.record_count() == 2
        assert restarted.conflict_count() == 2  # survives backend process restart
        forged = bytearray(periodic)
        forged[-1] ^= 1
        assert ledger.ingest(bytes(forged)).state == secure.LedgerState.REJECTED
        assert restarted.record_count() == 2
        with patch.object(ledger, "_matches", return_value=False):
            assert ledger.ingest(reencrypt(event, 105)).state == secure.LedgerState.UNCERTAIN
        assert restarted.record_count() == 2
        with sqlite3.connect(db) as conn:
            value = conn.execute(
                "SELECT plaintext FROM verified_observations WHERE sequence=257"
            ).fetchone()[0]
        assert bytes(value) == public_fixture("kPeriodicPlaintextHex")

        # The DB can have an intact plaintext/dedup key but damaged
        # original ciphertext. A repeat MUST NOT be treated as verified.
        with sqlite3.connect(db) as conn:
            damaged = bytearray(periodic)
            damaged[-1] ^= 0x01
            conn.execute(
                "UPDATE verified_observations SET exact_first_frame=?"
                " WHERE device_id=? AND sequence=257",
                (sqlite3.Binary(bytes(damaged)), DEVICE),
            )
        assert restarted.ingest(periodic).state == secure.LedgerState.UNCERTAIN


def test_conflict_capacity_fails_closed() -> None:
    with tempfile.TemporaryDirectory() as folder:
        path = Path(folder) / "limited.db"
        ledger = secure.VerifiedLocalLedger(path, keys, max_conflicts=1)
        base = public_fixture("kPeriodicFrameHex")
        assert ledger.ingest(base).state == secure.LedgerState.VERIFIED_STORED
        plain = bytearray(secure.open_product_frame(base, keys).plaintext)
        plain[56:58] = (3900).to_bytes(2, "big")
        one = reencrypt(base, 110, bytes(plain))
        assert ledger.ingest(one).state == secure.LedgerState.CONTENT_CONFLICT
        assert ledger.ingest(reencrypt(base, 111, bytes(plain))).state == secure.LedgerState.CONTENT_CONFLICT
        plain[56:58] = (3800).to_bytes(2, "big")
        two = reencrypt(base, 112, bytes(plain))
        assert ledger.ingest(two).state == secure.LedgerState.UNCERTAIN
        assert ledger.conflict_count() == 1 and ledger.record_count() == 1
        for invalid in (0, -1, 100001, True, 1.5):
            try:
                secure.VerifiedLocalLedger(path, keys, max_conflicts=invalid)
            except ValueError:
                pass
            else:
                raise AssertionError("accepted unbounded/invalid conflict capacity")


def test_real_edge_http_backend_then_crypto_verify() -> None:
    with tempfile.TemporaryDirectory() as folder:
        root = Path(folder)
        edge_store = edge.EdgeDurableInbox(root / "edge.db")
        periodic = public_fixture("kPeriodicFrameHex")
        event = public_fixture("kEventFrameHex")
        assert edge_store.put(periodic).state == edge.PutState.COMMITTED
        assert edge_store.put(event).state == edge.PutState.COMMITTED

        with receiver(root / "untrusted_staging.db") as server:
            sender = ingress.LoopbackEdgeSender(edge_store, "127.0.0.1", server.port)
            transfer = sender.send_window(limit=4)
            assert transfer.staged_untrusted == 2
            assert transfer.state == ingress.TransferState.STAGED_UNTRUSTED

            # Backend trusted owner opens bytes from the actual second SQLite
            # store, validates AEAD and commits stable logical identity.
            verified = secure.VerifiedLocalLedger(root / "verified.db", keys)
            snapshot = server.store.oldest_batch()
            assert snapshot.state == edge.ScanState.READY
            for obj in snapshot.items:
                assert verified.ingest(obj.ciphertext).state == secure.LedgerState.VERIFIED_STORED
            assert verified.record_count() == 2

            sender.send_window()  # replay lost response, still no release
            assert server.store.retained_count() == 2
            assert verified.ingest(periodic).state == secure.LedgerState.VERIFIED_DUPLICATE
            assert verified.record_count() == 2
            assert edge_store.retained_count() == 2


def main() -> None:
    test_golden_vectors()
    test_tag_header_key_and_semantic_refusal()
    test_ledger_logical_idempotence_and_conflict()
    test_conflict_capacity_fails_closed()
    test_real_edge_http_backend_then_crypto_verify()
    print("SF5H backend crypto ledger: AESCCM-KAT/semantic-id/dedup/conflict/live-HTTP: PASS")


if __name__ == "__main__":
    main()
