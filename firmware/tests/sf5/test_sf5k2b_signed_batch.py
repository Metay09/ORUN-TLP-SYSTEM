#!/usr/bin/env python3
"""SF5K2B: real SF5I Ed25519 -> host Gateway candidate selection.

Tests do not create custody ACK, product grants, or Tracker releases.
"""
from __future__ import annotations

from dataclasses import replace
from pathlib import Path
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "edge" / "candidates"))
import edge_durable_inbox as storage
import edge_durable_accept_signed as signed
import gateway_signed_batch_staging as staging
from test_sf5i_edge_durable_accept_signed import pending, setup, sf5b_frame


def window():
    names = ("kPeriodicFrameHex", "kEventFrameHex",
             "kKatPeriodicFrameHex", "kKatEventFrameHex")
    return tuple(replace(pending(sf5b_frame(name)),
                         attempt_id=0x10203040 + i,
                         source_slot=i, source_generation=19 + i)
                 for i, name in enumerate(names))


def scope(frames):
    f = frames[0]
    return staging.CandidateScope(
        tracker_id=int.from_bytes(f.exact_frame[8:16], "big"),
        product_key_epoch=int.from_bytes(f.exact_frame[16:20], "big"),
        gateway_instance=f.gateway_instance, edge_issuer=f.edge_issuer)


def test_signed_window_restart() -> None:
    with tempfile.TemporaryDirectory() as folder:
        store, signer, verifier, grant, secret = setup(folder)
        records = window()
        receipts = tuple(signer.accept(p).receipt for p in records)
        assert all(receipts)
        result = staging.stage_signed_edge_candidates(scope(records), records,
                                                       receipts, verifier)
        assert result.state == staging.StageState.CANDIDATES_ONLY
        assert tuple(m.input_index for m in result.members) == (0, 1, 2, 3)
        assert tuple(m.exact_frame for m in result.members) == tuple(
            p.exact_frame for p in records)
        assert store.retained_count() == 4
        reopened = storage.EdgeDurableInbox(store.path)
        resigner = signed.EdgeDurableAcceptSigner(reopened, grant, secret)
        assert tuple(resigner.accept(p).receipt for p in records) == receipts
        assert reopened.retained_count() == 4
        assert staging.stage_signed_edge_candidates(
            scope(records), records, receipts, verifier) == result


def test_selective_signed_only() -> None:
    with tempfile.TemporaryDirectory() as folder:
        store, signer, verifier, _, _ = setup(folder)
        records = window()
        one = signer.accept(records[0]).receipt
        three = signer.accept(records[2]).receipt
        fake = bytearray(three); fake[-1] ^= 1
        result = staging.stage_signed_edge_candidates(
            scope(records), records, (one, b"HTTP 200 OK", three, bytes(fake)),
            verifier)
        assert result.state == staging.StageState.CANDIDATES_ONLY
        assert tuple(m.input_index for m in result.members) == (0, 2)
        assert store.retained_count() == 2
        assert staging.stage_signed_edge_candidates(
            scope(records), records, (None, b"HTTP 200", None, bytes(fake)),
            verifier).state == staging.StageState.NO_SIGNED_DURABLE_EVIDENCE
        # Signed receipt for a DIFFERENT exact frame or attempt is not proof.
        wrong = list(records)
        wrong[0] = replace(records[0], attempt_id=records[0].attempt_id + 100)
        assert staging.stage_signed_edge_candidates(
            scope(records), tuple(wrong), (one, None, None, None),
            verifier).state == staging.StageState.NO_SIGNED_DURABLE_EVIDENCE


def test_uncertain_commit_and_late_local_fallback() -> None:
    with tempfile.TemporaryDirectory() as folder:
        store, signer, verifier, _, _ = setup(folder)
        only = (window()[0],)
        p = only[0]
        before = signer.accept(p, fault="before_commit")
        assert before.state == signed.SignState.STORE_UNCERTAIN
        assert not before.receipt
        after = signer.accept(p, fault="after_commit")
        assert after.state == signed.SignState.STORE_UNCERTAIN
        assert not after.receipt and store.retained_count() == 1
        assert staging.stage_signed_edge_candidates(
            scope(only), only, (after.receipt,),
            verifier).state == staging.StageState.NO_SIGNED_DURABLE_EVIDENCE
        recovery = signer.accept(p)
        assert recovery.state == signed.SignState.SIGNED_CANDIDATE
        assert staging.stage_signed_edge_candidates(
            scope(only), only, (recovery.receipt,),
            verifier).state == staging.StageState.CANDIDATES_ONLY
        late = (replace(p, local_fallback_started=True),)
        assert staging.stage_signed_edge_candidates(
            scope(only), late, (recovery.receipt,),
            verifier).state == staging.StageState.NO_SIGNED_DURABLE_EVIDENCE
        assert signer.accept(late[0]).state == signed.SignState.REFUSED


def test_reject_ambiguous_scope_and_duplicates() -> None:
    with tempfile.TemporaryDirectory() as folder:
        _, signer, verifier, _, _ = setup(folder)
        records = window()
        receipts = tuple(signer.accept(p).receipt for p in records)
        base = scope(records)
        bad = staging.StageState.INVALID
        assert staging.stage_signed_edge_candidates(
            replace(base, tracker_id=base.tracker_id + 1), records, receipts,
            verifier).state == bad
        assert staging.stage_signed_edge_candidates(
            replace(base, product_key_epoch=base.product_key_epoch + 1),
            records, receipts, verifier).state == bad
        assert staging.stage_signed_edge_candidates(
            replace(base, edge_issuer=base.edge_issuer + 1), records, receipts,
            verifier).state == bad
        assert staging.stage_signed_edge_candidates(
            base, records + (records[0],), receipts + (receipts[0],),
            verifier).state == bad
        assert staging.stage_signed_edge_candidates(
            base, records, receipts[:-1], verifier).state == bad
        assert staging.stage_signed_edge_candidates(
            base, (records[0], records[0]), receipts[:2], verifier).state == bad
        ambiguous_attempt = replace(records[1], attempt_id=records[0].attempt_id)
        assert staging.stage_signed_edge_candidates(
            base, (records[0], ambiguous_attempt), receipts[:2], verifier).state == bad
        ambiguous_slot = replace(records[1], source_page=records[0].source_page,
                                 source_slot=records[0].source_slot,
                                 source_generation=records[0].source_generation)
        assert staging.stage_signed_edge_candidates(
            base, (records[0], ambiguous_slot), receipts[:2], verifier).state == bad


def main() -> None:
    test_signed_window_restart()
    test_selective_signed_only()
    test_uncertain_commit_and_late_local_fallback()
    test_reject_ambiguous_scope_and_duplicates()
    print("SF5K2B signed Edge -> Gateway batch staging (host only): PASS")


if __name__ == "__main__":
    main()
