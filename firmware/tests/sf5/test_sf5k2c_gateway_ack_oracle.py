#!/usr/bin/env python3
"""SF5K2C host-only: signed SF5I Edge -> Gateway test-ACK -> SF5K1 oracle.

Executable when checked out in ORUN repo; uses real Ed25519/SQLite from SF5I.
Never transmits, accepts production authority, or releases Tracker records.
"""
from __future__ import annotations

from dataclasses import replace
from pathlib import Path
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "edge" / "candidates"))
import edge_durable_accept_signed as signed
import edge_durable_inbox as inbox
import sf5k2c_gateway_ack_oracle as composer
import test_sf5k1_batch_mac_reference as oracle
from test_sf5k2b_signed_batch import window, scope, setup

TEST_KEY = bytes.fromhex("9cbae4ff1a87b6924a29787038b165c1a1113b618d7ad883a9007f044d992d14")


def grant():
    return composer.TestOnlyGatewayGrant(
        gateway_id=0x2112131415161718,
        policy_floor=0x10203040,
        grant_generation=0x44556677,
        active=True,
    )


def tracker_fixture_scope(records):
    s = scope(records)
    g = grant()
    return (s.tracker_id, g.gateway_id, s.product_key_epoch,
            g.policy_floor, g.grant_generation)


def compose(records, receipts, verifier, *, granted=None, key=TEST_KEY):
    return composer.compose_host_fixture_ack(
        scope(records), records, receipts, verifier,
        grant() if granted is None else granted, key)


def test_all_counts_signed_committed_and_readback():
    for n in (1, 2, 3, 4):
        with tempfile.TemporaryDirectory() as folder:
            store, signer, verifier, _, _ = setup(folder)
            records = window()[:n]
            receipts = tuple(signer.accept(p).receipt for p in records)
            assert all(receipts) and store.retained_count() == n
            wire = compose(records, receipts, verifier)
            assert wire and len(wire) == 48 + 9 * n
            exact = [p.exact_frame for p in records]
            assert oracle.preflight_untrusted_host_fixture(
                wire, exact, TEST_KEY,
                {"active": True, "scope": tracker_fixture_scope(records)}) is not None
            expected = oracle.test_only_sign(
                exact, key=TEST_KEY,
                tracker=scope(records).tracker_id,
                gateway=grant().gateway_id,
                epoch=scope(records).product_key_epoch,
                floor=grant().policy_floor,
                grant=grant().grant_generation)
            assert wire == expected, "independent SF5K1 byte parity"
            reread = inbox.EdgeDurableInbox(store.path)
            assert reread.retained_count() == n
            assert compose(records, receipts, verifier) == wire


def test_missing_forged_and_partial_are_selective():
    with tempfile.TemporaryDirectory() as folder:
        store, signer, verifier, _, _ = setup(folder)
        records = window()
        first = signer.accept(records[0]).receipt
        third = signer.accept(records[2]).receipt
        forged = bytearray(third); forged[-1] ^= 1
        receipts = (first, b"HTTP 200 OK", third, bytes(forged))
        wire = compose(records, receipts, verifier)
        assert wire is not None and len(wire) == 66
        selected = oracle.preflight_untrusted_host_fixture(
            wire, [p.exact_frame for p in records], TEST_KEY,
            {"active": True, "scope": tracker_fixture_scope(records)})
        assert selected is not None and set(selected) == {0, 2}
        assert store.retained_count() == 2
        assert compose(records, (None, None, None, None), verifier) is None
        changed = bytearray(wire); changed[-1] ^= 1
        assert oracle.preflight_untrusted_host_fixture(
            bytes(changed), [p.exact_frame for p in records], TEST_KEY,
            {"active": True, "scope": tracker_fixture_scope(records)}) is None
        assert oracle.preflight_untrusted_host_fixture(
            wire, [records[1].exact_frame, records[3].exact_frame], TEST_KEY,
            {"active": True, "scope": tracker_fixture_scope(records)}) is None


def test_missing_authority_uncertain_and_stale():
    with tempfile.TemporaryDirectory() as folder:
        store, signer, verifier, _, _ = setup(folder)
        records = window()[:1]
        p = records[0]
        uncertain = signer.accept(p, fault="after_commit")
        assert uncertain.state == signed.SignState.STORE_UNCERTAIN
        assert not uncertain.receipt and store.retained_count() == 1
        assert compose(records, (uncertain.receipt,), verifier) is None
        signed_after_reconciliation = signer.accept(p)
        receipt = signed_after_reconciliation.receipt
        assert receipt
        assert compose(records, (receipt,), verifier, granted=replace(grant(), active=False)) is None
        assert compose(records, (receipt,), verifier, granted=replace(grant(), grant_generation=0)) is None
        assert compose(records, (receipt,), verifier, key=b"bad") is None
        wrong_attempt = replace(p, attempt_id=p.attempt_id + 1)
        assert compose((wrong_attempt,), (receipt,), verifier) is None
        pending_fallback = replace(p, local_fallback_started=True)
        assert compose((pending_fallback,), (receipt,), verifier) is None
        original = compose(records, (receipt,), verifier)
        assert original
        assert oracle.preflight_untrusted_host_fixture(
            original, [p.exact_frame], TEST_KEY,
            {"active": False, "scope": tracker_fixture_scope(records)}) is None
        assert oracle.preflight_untrusted_host_fixture(
            original, [p.exact_frame], TEST_KEY,
            {"active": True, "scope": (scope(records).tracker_id,
                grant().gateway_id + 1, scope(records).product_key_epoch,
                grant().policy_floor, grant().grant_generation)}) is None


def test_duplicate_or_invalid_batch_never_generates_ack():
    with tempfile.TemporaryDirectory() as folder:
        _, signer, verifier, _, _ = setup(folder)
        p = window()[0]
        receipt = signer.accept(p).receipt
        assert compose((p, p), (receipt, receipt), verifier) is None
        wrong = replace(p, source_slot=32)
        assert compose((wrong,), (receipt,), verifier) is None
        altered = bytearray(p.exact_frame); altered[-1] ^= 1
        assert compose((replace(p, exact_frame=bytes(altered)),), (receipt,), verifier) is None
        assert compose((p,), (receipt, receipt), verifier) is None


def main():
    test_all_counts_signed_committed_and_readback()
    test_missing_forged_and_partial_are_selective()
    test_missing_authority_uncertain_and_stale()
    test_duplicate_or_invalid_batch_never_generates_ack()
    print("SF5K2C signed Edge -> test-only Gateway batch HMAC -> SF5K1: PASS")


if __name__ == "__main__":
    main()
