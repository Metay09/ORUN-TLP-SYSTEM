# SF5G — Read-only reclaim evidence review (host-only)

Date: 2026-10-14
Status: **CANDIDATE; NEW HEAD HOST TESTS PENDING**.
Base: `main@33a29df494df6cde17a277df912397a279713444`
(PR #123 reclaim intent **census** already merged).

## Purpose

A committed/handed-off flash marker, valid CRC, or structurally completed
reclaim intent is **not an authenticated durable Edge receipt**. We must
never erase the only copy of an unsettled ciphertext just because a flag
looks complete. This host-only slice adds an **advisory, read-only
reclaim evidence coverage classifier**. It does **NOT** produce an erase
permit/capability; future runtime must implement verified receipt
cryptography, ownership, correct durable custody transfers and crash-safe
reclamation independently.

New `firmware/tests/sf5/` files:

* `sf5g_reclaim_evidence_candidate.h/.cpp`
* `test_sf5g_reclaim_evidence_candidate.cpp`

Integrated into `firmware/tests/run_host_tests.sh`. No changes to
`firmware/src`, `firmware/include`, `platformio.ini`, existing
hardware media or radio/ACK code.

## Conservative structural policy

`reviewPage(NorPage, page index, actual page count, ReceiptSource)`
inspects one complete 4 KiB page snapshot and refuses
invalid count/index, non-ACTIVE/foreign/torn OCP3 header, nonerased
reserved tail (guard + intents), corrupt/staged/torn records, impossible
handoff ordering, and any gap in the record sequence.

Every record slot must be occupied (32/32) and **all** must be
structurally `kHandedOff` in canonical marker order:
A = fully committed, B = erased or fully committed. Even a single
`kHeld` record blocks the candidate. For each slot, an externally
provided `ReceiptSource` is queried using **page index, generation,
slot index, exact 84/100 ciphertext bytes and exact length**. Absent,
invalid, stale, misbound, wrong-length or altered-object claims block
the candidate. The checker calls the mock only **after the full
structural scan**, so an invalid slot cannot be partially certified
before it is discovered.

Only if every slot is accounted for, the review result is
`kCandidateOnly`, *not* `kSafeToErase` or `kReadyToReclaim`.
`ReceiptSource::kClaimsDurableExactObject` is a **caller-supplied
assertion**, not cryptographic verification. The tests use an in-memory
mock and therefore cannot establish authenticity, replay protection,
end-to-end Edge durability, or actual right to erase.

Other review outcomes: `kNotFull`, `kHeld`,
`kMissingReceipt`, `kRefused`, `kReadFailure`.
All deny erase. The API contains **no flash mutation function**.

## Host test matrix

- Full 32-record page: HELD despite externally accepted mock claims.
- Last remaining HELD slot blocks candidacy.
- All handed off but no receipts: missing; only all *exactly*
  page/generation/slot/bytes/length-bound claims produce `kCandidateOnly`.
- Missing one, wrong page, stale generation, modified exact ciphertext
  and mock-invalid claim fail closed.
- Torn secondary handoff marker, impossible B-only handoff, corrupted
  committed record, and occupied intent area: refuse.
- Empty or partially filled page cannot be a candidate.
- Read fault, foreign OCQ1 header, invalid physical source/count: refuse.

## Unresolved engineering gates

- Current `ReceiptSource` is a mock contract. Implement real
  authenticated receipt validation, trusted Edge identity, durable
  acceptance and replay resistance. Local object CRC is not security.
- Distinguish Gateway-to-Edge accepted-into-durable-store from
  tracker-to-Gateway custody ACK and radio transmission; neither a
  handoff bit nor just a successful send releases custody.
- Enforce product_region ownership, source/target generations and
  region-wide FIFO and duplicate policies; ensure a stalled source
  does not cause unreadable/unreclaimable media.
- Design spare-page relocation and reclaim journal semantics for mixed
  settled + HELD records, without losing the 8 HELD records in an
  otherwise 24-settled example. This isolated review is deliberately
  stricter: it rejects any mixed page.
- Implement crash-safe erase/rotation with interrupted NOR erase
  injection, async FlashBackend, readback, N1-N5, physical validation,
  and a storage-critical independent audit before device activation.
- No actual physical power cut, Gateway runtime integration, radio
  custody ACK or hardware flash operation exists here.

**Verification gate:** run full host tests on the exact new branch HEAD
and check for seven SF5G PASS lines; do not merge before that result.
