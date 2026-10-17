# SF5K3G — bounded read-only real-FlashBackend v3 A/B preflight

**Status: DRAFT / not yet compiled or run by the user.**
**This is NOT a production v3 migration, recovery activation or writer.**

## What changed

- `security_v3_format` now exposes `JournalReader` and
  `recoverCommittedJournalStream()` so embedded flash scan uses a
  fixed 56-byte record scratch buffer. The existing pointer-based
  journal decoder delegates to the exact same validated parser.
- `security_v3_ab_preflight.h/.cpp` adds a production-source
  **read-only** physical A/B recovery/election preflight that consumes
  the existing initialized, quiescent `FlashBackend` and the two
  4096-byte SecurityStore security pages.
- Separate SF5K3G host test runs this actual FlashBackend path with
  controlled fault injection and independent SF5K3D snapshots.
- Host CI runs it in the focused and full regression suites.

## Exact security boundary

The existing `SecurityStore::begin/recover/poll` is unchanged.
It still accepts v1/v2 as active authority and writes v2. The new
preflight is **not** wired into boot, application networking, ACK
verification, FlashMutationGate mutation, provisioning, or
`ProductRecordStore::release`. It is deliberately invoked only
by host tests / future explicit read-only diagnostics. It cannot
set `SecurityStore::state_`, install `K_root`, raise counters,
register Gateway IDs or grant custody.

`security_v3_ab_preflight::inspect(flash, tracker_id, summary)`
requires a pre-initialized **quiescent** FlashBackend (no pending
erase/program/SoftDevice event and no concurrent writer) and performs
read() only, using bounded stack buffers. It returns a single
result `kUnprovisioned/kV1/kV2/kV3/kUnsupported/kForeign/kFault/
kReadFault` and publishes only scalar summary on a valid selected
page. No root secrets or Gateway IDs are exposed.

## Checked election invariants

- Only header-committed, CRC-correct, credential-bound, page-local
  records may enter A/B selection. v3 requires all five opening
  snapshot entries and a fully checked append sequence.
- An unknown future ORS1 header blocks downgrade; a committed corrupt
  page, foreign credential or physical read failure cannot silently
  fall back to the other page.
- Highest page generation wins only if generations are unambiguous.
  For the same root and credential ID, TX/A2D bounds, policy
  revision/floor, GLOBAL grant-generation high-water and EACH
  Gateway slot's own generation cannot roll back. Same slot
  generation also requires unchanged Gateway identity. Equal policy
  revisions require the identical complete policy snapshot.
- A root rotation requires BOTH a changed credential ID and changed
  root. Only a new credential's empty Gateway policy is allowed in the
  newer page; the numeric epoch is allowed to be equal.
- v3 -> v1/v2 is a forbidden downgrade even when v2 header generation
  is numerically higher. Stage/power-cut appends with missing commit
  do not replay; partial commit words, log holes, dirty tails,
  malformed counter blocks and unexpected entry kinds fail closed.
- Outputs are untouched on all error / empty cases; the fake backend
  asserts no program or erase calls.

**Known remaining gap:** This pairwise preflight does not itself
prove how to distinguish every physically interrupted old-page erase
from every incomplete new-page build; production A/B writer and
recovery must share a reviewed physical transaction protocol before
this could become authoritative. A staged page is never used as
authority. Partial old-page erase, race with pending flash completion,
credential rotation and same-root compaction must be tested through
the real `SecurityStore` and `FlashMutationGate` together.

## Validation commands

On `feat/sf5k3g-security-v3-ab-preflight`:

```bash
cd ~/ORUN-TLP-SYSTEM
bash tools/ci/run_sf5k3a_host.sh
bash firmware/tests/run_host_tests.sh
cd firmware && pio run -e rak4630
```

**No PASS is claimed until the commands run.** The target compilation
does not flash a device. Retain PR as DRAFT; runtime v3 authority,
Gateway enrollment, authenticated custody ACK and record release
must remain off.
