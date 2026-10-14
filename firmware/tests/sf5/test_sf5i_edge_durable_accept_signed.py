#!/usr/bin/env python3
"""SF5I Ed25519 Edge->Gateway exact-object acceptance, host-only.

Real Ed25519 keys are generated ephemerally. No real provisioning credentials
or production receipt bytes are committed or used on hardware.
"""
from __future__ import annotations

from dataclasses import replace
from pathlib import Path
import re
import sqlite3
import sys
import tempfile
from unittest.mock import patch

from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "edge" / "candidates"))
import edge_durable_inbox as storage
import edge_durable_accept_signed as accept


def sf5b_frame(name: str) -> bytes:
    text = (ROOT / "firmware/tests/sf5/sf5b_product_vectors.h").read_text()
    m = re.search(
        r"static const char " + re.escape(name) +
        r"\[\]\s*=\s*((?:\s*\"[0-9a-f]+\"\s*)+);", text,
    )
    assert m
    return bytes.fromhex("".join(re.findall(r'\"([0-9a-f]+)\"', m.group(1))))


def pending(frame: bytes) -> accept.PendingCustody:
    return accept.PendingCustody(
        gateway_instance=0x1122334455667788,
        attempt_id=0x10203040,
        edge_issuer=0x0102030405060708,
        source_page=2,
        source_slot=7,
        source_generation=19,
        exact_frame=frame,
    )


def setup(folder: str, max_objects: int = 8):
    db = Path(folder) / "edge.sqlite"
    store = storage.EdgeDurableInbox(db, max_objects=max_objects)
    grant = accept.EdgeEnrollment(
        edge_issuer=0x0102030405060708,
        gateway_instance=0x1122334455667788,
        edge_key_epoch=4,
        grant_generation=11,
    )
    secret = Ed25519PrivateKey.generate()
    signer = accept.EdgeDurableAcceptSigner(store, grant, secret)
    verifier = accept.GatewayEdgeReceiptVerifier(grant, secret.public_key())
    return store, signer, verifier, grant, secret


def test_proof_after_commit_and_reboot() -> None:
    with tempfile.TemporaryDirectory() as folder:
        store, signer, verifier, grant, secret = setup(folder)
        a = pending(sf5b_frame("kEventFrameHex"))
        b = replace(a, attempt_id=a.attempt_id + 1,
                    exact_frame=sf5b_frame("kPeriodicFrameHex"))
        r = signer.accept(a)
        assert r.state == accept.SignState.SIGNED_CANDIDATE and len(r.receipt) > 100
        assert verifier.matches(a, r.receipt)
        assert store.retained_count() == 1
        again = signer.accept(a)
        assert again.state == accept.SignState.SIGNED_CANDIDATE
        assert again.receipt == r.receipt  # replay same exact accepted attempt is idempotent
        assert store.retained_count() == 1

        r2 = signer.accept(b)
        assert r2.state == accept.SignState.SIGNED_CANDIDATE
        assert verifier.matches(b, r2.receipt)
        assert store.retained_count() == 2
        assert not verifier.matches(a, r2.receipt)

        # No signing credential needs to live in the SQLite database.
        reopen = storage.EdgeDurableInbox(store.path)
        recovered = accept.EdgeDurableAcceptSigner(reopen, grant, secret)
        after_restart = recovered.accept(a)
        assert after_restart.state == accept.SignState.SIGNED_CANDIDATE
        assert after_restart.receipt == r.receipt
        assert verifier.matches(a, after_restart.receipt)
        assert reopen.retained_count() == 2


def test_identity_key_grant_and_object_binding() -> None:
    with tempfile.TemporaryDirectory() as folder:
        store, signer, verifier, grant, secret = setup(folder)
        original = pending(sf5b_frame("kPeriodicFrameHex"))
        signed = signer.accept(original)
        assert signed.state == accept.SignState.SIGNED_CANDIDATE
        assert verifier.matches(original, signed.receipt)

        tampered_claims = [
            replace(original, gateway_instance=original.gateway_instance + 1),
            replace(original, attempt_id=original.attempt_id + 1),
            replace(original, edge_issuer=original.edge_issuer + 1),
            replace(original, source_page=original.source_page + 1),
            replace(original, source_slot=original.source_slot + 1),
            replace(original, source_generation=original.source_generation + 1),
            replace(original, exact_frame=sf5b_frame("kKatPeriodicFrameHex")),
            replace(original, local_fallback_started=True),
        ]
        for changed in tampered_claims:
            assert not verifier.matches(changed, signed.receipt)
        for delta in [
            {"edge_key_epoch": grant.edge_key_epoch + 1},
            {"grant_generation": grant.grant_generation + 1},
            {"gateway_instance": grant.gateway_instance + 1},
            {"edge_issuer": grant.edge_issuer + 1},
        ]:
            wrong = accept.GatewayEdgeReceiptVerifier(replace(grant, **delta),
                                                      secret.public_key())
            assert not wrong.matches(original, signed.receipt)
        other = Ed25519PrivateKey.generate()
        assert not accept.GatewayEdgeReceiptVerifier(
            grant, other.public_key()).matches(original, signed.receipt)
        changed_signature = bytearray(signed.receipt)
        changed_signature[-2] ^= 0x20
        assert not verifier.matches(original, bytes(changed_signature))
        changed_header = bytearray(signed.receipt)
        changed_header[2] ^= 0x01
        assert not verifier.matches(original, bytes(changed_header))
        assert not verifier.matches(original, b"HTTP 200 OK")
        assert not verifier.matches(original, signed.receipt[:-1])
        assert not verifier.matches(original, signed.receipt + b"\x00")
        assert store.retained_count() == 1


def test_uncertain_commit_full_corruption_no_proof() -> None:
    with tempfile.TemporaryDirectory() as folder:
        store, signer, verifier, grant, secret = setup(folder, 1)
        event = pending(sf5b_frame("kEventFrameHex"))
        periodic = replace(event, attempt_id=event.attempt_id + 1,
                           exact_frame=sf5b_frame("kPeriodicFrameHex"))

        before = signer.accept(event, fault="before_commit")
        assert before.state == accept.SignState.STORE_UNCERTAIN and not before.receipt
        assert store.retained_count() == 0
        after = signer.accept(event, fault="after_commit")
        assert after.state == accept.SignState.STORE_UNCERTAIN and not after.receipt
        assert store.retained_count() == 1
        recovered = signer.accept(event)
        assert recovered.state == accept.SignState.SIGNED_CANDIDATE
        assert verifier.matches(event, recovered.receipt)

        full = signer.accept(periodic)
        assert full.state == accept.SignState.STORE_UNCERTAIN and not full.receipt
        assert store.retained_count() == 1

        with patch.object(store, "_readback", return_value=False):
            no_readback = signer.accept(event)
            assert no_readback.state == accept.SignState.STORE_UNCERTAIN
            assert no_readback.receipt == b""

        with sqlite3.connect(store.path) as db:
            changed = bytearray(event.exact_frame)
            changed[39] ^= 0x01
            db.execute("UPDATE exact_frames SET ciphertext=? WHERE sequence=1",
                       (sqlite3.Binary(bytes(changed)),))
        no_proof = signer.accept(event)
        assert no_proof.state == accept.SignState.STORE_UNCERTAIN
        assert not no_proof.receipt


def test_invalid_request_and_no_issuer_signature() -> None:
    with tempfile.TemporaryDirectory() as folder:
        store, signer, _, grant, _ = setup(folder)
        valid = pending(sf5b_frame("kEventFrameHex"))
        for invalid in [
            replace(valid, edge_issuer=9),
            replace(valid, gateway_instance=3),
            replace(valid, attempt_id=0),
            replace(valid, source_page=32),
            replace(valid, source_slot=-1),
            replace(valid, source_generation=0),
            replace(valid, local_fallback_started=True),
            replace(valid, exact_frame=b"spoof"),
        ]:
            r = signer.accept(invalid)
            assert r.state == accept.SignState.REFUSED and not r.receipt
        assert store.retained_count() == 0

        for invalid_grant in [
            replace(grant, edge_key_epoch=0),
            replace(grant, grant_generation=0),
            replace(grant, edge_issuer=0),
        ]:
            try:
                accept.EdgeDurableAcceptSigner(store, invalid_grant,
                                               Ed25519PrivateKey.generate())
            except ValueError:
                pass
            else:
                raise AssertionError("invalid enrollment accepted")


def main() -> None:
    test_proof_after_commit_and_reboot()
    test_identity_key_grant_and_object_binding()
    test_uncertain_commit_full_corruption_no_proof()
    test_invalid_request_and_no_issuer_signature()
    print("SF5I Edge signed durable-accept: Ed25519/exact-frame/restart/fault/fallback/no-release: PASS")


if __name__ == "__main__":
    main()
