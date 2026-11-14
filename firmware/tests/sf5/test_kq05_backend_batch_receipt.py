#!/usr/bin/env python3
"""KQ05: real Backend AEAD + committed SQLite + HKDF/HMAC selective receipts.

Host-only security candidate. Test keys are PUBLIC, never provision a RAK.
"""
from __future__ import annotations

from pathlib import Path
import sys
import sqlite3
import tempfile
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "backend" / "candidates"))
sys.path.insert(0, str(ROOT / "firmware" / "tests" / "sf5"))
import product_secure_verified_ledger as ledger
import kq05_backend_batch_receipt as batch
from test_sf5h_backend_verified_ledger import (
    CRED, DEVICE, EPOCH, keys, public_fixture, reencrypt,
)


def rows(n: int) -> tuple[bytes, ...]:
    original = public_fixture("kPeriodicFrameHex")
    opened = ledger.open_product_frame(original, keys)
    frames = []
    for i in range(n):
        p = bytearray(opened.plaintext)
        p[12:16] = (101 + i).to_bytes(4, "big")
        p[16:20] = (1700000000 + 60 * i).to_bytes(4, "big")
        template = bytearray(original)
        template[4] = 0  # ACKless direct LIVE AAD
        # reencrypt reads the original, preserves its plaintext, and needs
        # an explicit plaintext for our different logical observation.
        frames.append(reencrypt(bytes(template), 3000 + i, bytes(p), flags=0))
    return tuple(frames)


def _reject_auth(wire: bytes, incarnation: bytes) -> None:
    assert batch.verify_for_tracker(wire, CRED, DEVICE, incarnation, EPOCH) is None


def test_receipt_selective_scope_tamper_and_reboot() -> None:
    with tempfile.TemporaryDirectory() as d:
        path = Path(d) / "verified.db"
        store = ledger.VerifiedLocalLedger(path, keys)
        values = rows(6)
        selected = (0, 1, 3, 4)  # missing 103 on purpose
        for i in selected:
            assert store.ingest(values[i]).state == ledger.LedgerState.VERIFIED_STORED
        inc = ledger.open_product_frame(values[0], keys).incarnation
        issuer = batch.BackendBatchIssuer(store)
        expected = (101, 102, 104, 105)
        signed = issuer.issue(DEVICE, inc, EPOCH, expected)
        assert signed.state == batch.State.SIGNED
        assert len(signed.wire) == 24 + 4 * len(expected) + 16
        assert batch.verify_for_tracker(signed.wire, CRED, DEVICE, inc, EPOCH) == expected
        assert issuer.issue(DEVICE, inc, EPOCH, (103,)).state == batch.State.NOT_DURABLE
        assert issuer.issue(DEVICE, inc, EPOCH, (101, 103)).state == batch.State.NOT_DURABLE
        assert issuer.issue(DEVICE, inc, EPOCH, (105, 104)).state == batch.State.INVALID
        assert issuer.issue(DEVICE, inc, EPOCH, (101, 101)).state == batch.State.INVALID
        assert issuer.issue(DEVICE, inc, EPOCH, tuple(range(101, 110))).state == batch.State.INVALID
        assert issuer.issue(DEVICE, bytes(8), EPOCH, expected).state == batch.State.INVALID
        assert issuer.issue(DEVICE, inc, EPOCH + 1, expected).state == batch.State.UNAVAILABLE

        restarted = ledger.VerifiedLocalLedger(path, keys)
        again = batch.BackendBatchIssuer(restarted).issue(DEVICE, inc, EPOCH, expected)
        assert again == signed  # restart-safe persistent logical proof
        assert batch.verify_for_tracker(signed.wire, CRED, DEVICE, inc, EPOCH) == expected

        for i in range(len(signed.wire)):
            bad = bytearray(signed.wire)
            bad[i] ^= 1
            _reject_auth(bytes(bad), inc)
        assert batch.verify_for_tracker(signed.wire, CRED, DEVICE, b"WRONGINC", EPOCH) is None
        assert batch.verify_for_tracker(signed.wire, CRED, DEVICE, inc, EPOCH + 1) is None
        wrong = ledger.BackendCredential(CRED.credential_id, bytes(32))
        assert batch.verify_for_tracker(signed.wire, wrong, DEVICE, inc, EPOCH) is None

        # Full ACK evidence must still fail if the stored canonical frame is
        # corrupted or a conflicting authenticated plaintext was committed.
        with sqlite3.connect(path) as db:
            raw = bytearray(values[0])
            raw[-1] ^= 1
            db.execute(
                "UPDATE verified_observations SET exact_first_frame=? "
                "WHERE device_id=? AND incarnation=? AND sequence=101",
                (sqlite3.Binary(bytes(raw)), DEVICE, inc),
            )
        assert issuer.issue(DEVICE, inc, EPOCH, expected).state == batch.State.UNAVAILABLE


def test_conflict_and_uncertain_never_issue() -> None:
    with tempfile.TemporaryDirectory() as d:
        store = ledger.VerifiedLocalLedger(Path(d) / "verified.db", keys)
        vals = rows(2)
        assert store.ingest(vals[0]).state == ledger.LedgerState.VERIFIED_STORED
        inc = ledger.open_product_frame(vals[0], keys).incarnation
        issuer = batch.BackendBatchIssuer(store)
        assert issuer.issue(DEVICE, inc, EPOCH, (101,)).state == batch.State.SIGNED
        p = bytearray(ledger.open_product_frame(vals[0], keys).plaintext)
        p[56:58] = (3800).to_bytes(2, "big")
        alt = reencrypt(public_fixture("kPeriodicFrameHex"), 9900, bytes(p), flags=0)
        assert store.ingest(alt).state == ledger.LedgerState.CONTENT_CONFLICT
        assert issuer.issue(DEVICE, inc, EPOCH, (101,)).state == batch.State.NOT_DURABLE
        assert issuer.issue(DEVICE, inc, EPOCH, (102,)).state == batch.State.NOT_DURABLE
        with patch.object(store, "_conn", side_effect=sqlite3.OperationalError):
            assert issuer.issue(DEVICE, inc, EPOCH, (101,)).state == batch.State.UNAVAILABLE


def main() -> None:
    test_receipt_selective_scope_tamper_and_reboot()
    test_conflict_and_uncertain_never_issue()
    print("KQ05 backend durable selective batch HMAC/AESCCM/SQLite: PASS")


if __name__ == "__main__":
    main()
