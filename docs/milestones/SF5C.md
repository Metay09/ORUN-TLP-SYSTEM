# SF5C — Portable ObservationStore

Status: **THIRD INDEPENDENT AUDIT FAIL (6e5c306: 0 BLOCKER /
1 HIGH / 1 MEDIUM / 2 LOW). HIGH CONTROL-ERASE RECOVERY IS STILL OPEN.
FOLLOW-UP SAFETY GUARD/REGRESSION-TEST COMMITS ARE NOT VALIDATED.
PORTABLE STORAGE ONLY; NO nRF ADDRESS ALLOCATION, NO PRODUCTION RF
CUTOVER, NO PHYSICAL PASS. DO NOT MERGE.**

Baseline:
`main@5b757966489e5fe221c05fe1cbf66ff7b1e602d5` (SF5B merged).

Branch:
`feat/sf5c-observation-store`.

### 2026-10-08 — Third independent audit of `6e5c306`: FAIL

Independent Astra review reported **0 BLOCKER / 1 HIGH / 1 MEDIUM / 2 LOW**
with complete ASan/UBSan/-Werror host suite PASS and RAK4630 build PASS.
The RAK image has no linked ObservationStore symbols, so the build proves no
active runtime integration, not real flash durability. No physical test.

- **HIGH OPEN:** aborted erases of stale/PREPARED control pages can leave a
  suspected header whose generation/static CRC/activation proof was erased.
  Rejecting it faults the whole store despite a valid current authority; simply
  ignoring it can also roll back a corrupted newer ACTIVE authority. Prefix
  lengths 49..4096 and copied PREPARED targets remain unhandled. Header-prefix
  classification is not a reliable physical NVMC erase model.
- **MEDIUM TEST GAP:** existing rollback fixture used surviving g=1 and never
  called `sameLiveControlSnapshot()`; g=3/g=2 divergent authority cases and
  snapshot bypass mutations lacked regression coverage.
- **LOW #1:** treating pre-submission BUSY/token-timeout `kFailed` as an
  unreconciled submitted write sacrifices availability; fail-closed remains
  safer while FlashBackend cannot distinguish *never submitted* from a timed-out
  accepted operation.
- **LOW #2:** `begin()` while an async job is in flight discards the store's
  pending-operation ownership; reject reinitialization until `poll()` settles.

Unvalidated follow-up on same draft branch: guard `begin()` against in-flight
jobs, add an async ownership regression, and exercise an actually divergent
latest g=3 authority with surviving g=2 so removing snapshot matching must
cause test failure. These do **NOT** close HIGH control-erase recovery.

**Required H-1 design step:** persist a control-page erase/compaction intent
*before* erasing its target and bind the intent to the current authoritative
source and target. Make activation/intent retirement crash-safe so a stale
intent cannot cause rollback of a newer active journal. Define marker/program
partial-write semantics, source-vs-target arbitration, serial/custody invariants,
and brown-out recovery under arbitrary (not just prefix) torn erases before
coding a recovery shortcut. Exercise reset/power-cut at every phase, divergent
newer authority, old/new control copies, and fully erased targets. Preserve
frozen wire/golden fixtures. SF5D must physically qualify NVMC behavior and
flash wear after this portable design passes independent review.

### 2026-10-08 — Re-audit of `10d483c` (NOT CLOSED)

Independent Astra re-audit reported **FAIL: 0 BLOCKER / 1 HIGH / 1 MEDIUM /
1 LOW**, although full host and RAK4630 build passed on that exact SHA.

- **HIGH H2:** torn stale control-header prefixes 1–3 and 45–48, and a
  second power cut while erasing never-authoritative PREPARED control target,
  still caused persistent fault.
- **MEDIUM H3:** some public backlog/control reads remained accessible while
  backend `hasUnreconciledMutation()` was true before `poll()` faulting.
- **LOW:** a late SoC completion could clear backend timeout quarantine
  before the next store poll, permitting resubmission of the same mutation.

Subsequent same-branch remediation (not yet validated):
- bounded semantic-equivalence check of *latest live committed* control entries
  when the damaged ACTIVE header's generation/CRC is lost; require matching
  canonical serial/payload for all live keys before accepting fallback. Reject
  divergent newer control authority and ambiguous active-marker destruction;
- recovery of a never-ACTIVE PREPARED target only while its erased activation
  word is intact, including two successive interrupted erases;
- every byte prefix from 1 through 48 covered on stale and PREPARED pages;
- block public backlog counts, oldest retention, open EVENT and RESULT lookups
  as well as existing exact-object and record lookup while backend uncertainty
  exists; preserve a local uncertainty latch even if late hardware completion
  clears the gate quarantine before the next store poll.

No TLP v1, SF2/SF3, radio, on-device flash allocation or golden fixture
changes are authorized by these fixes. **Do not merge** before a new focused
host run, full host regressions, PlatformIO RAK4630 build and independent
review of the new commit. The higher-read-cost recovery comparison is confined
to the exceptional torn-header path; endurance still needs SF5D qualification.

### 2026-10-08 — Independent audit disposition (NOT CLOSED)

The independent review of `59ebe2308d03cc4eb96fc8507dfe947343b414b5`
returned **FAIL: 1 BLOCKER / 3 HIGH / 1 MEDIUM / 0 LOW**, despite that
earlier commit's complete host suite and RAK4630 build passing. Source details
and reproduction evidence are held in the reviewer's local
`/tmp/SF5C_INDEPENDENT_AUDIT_59ebe23.md` (not yet a repository artifact).

The same branch now contains targeted, **not-yet-retested** remediation:

- **BLOCKER:** carry cumulative retired-sequence high-water through every
  rotation, including a page with only staged writes, preventing identity reuse.
- **HIGH:** clear the old ACTIVE data-page pointer when rotation prepares it.
- **HIGH:** recover a torn, stale control-page erase only if a valid newer
  authority exists and the remaining stale-header suffix exactly matches the
  expected prior-generation header; other corruption remains fail-closed.
- **HIGH:** reject DATA/CONTROL success and public custody-object publication
  during a backend `hasUnreconciledMutation()` condition.
- **MEDIUM:** mutation RESULT admission binds the reserved guard's
  `command_id` to the committed v1 RESULT payload.

Added regression cases cover each report and authority-corruption rejection.
**Do not merge** until updated SF5C targets, full host ASan/UBSan + `-Werror`,
RAK4630 build and independent reviewer re-verification all pass. Earlier
`59ebe23` PASS reports must not be represented as evidence for the new head.
The control-page wear amplification measured by the independent reviewer
remains a required endurance/budget evaluation before physical SF5D activation.

Parent contracts:

- `docs/architecture/ORUN_TLP_V2_PRODUCT_OBSERVATION_STORAGE_CUTOVER.md`
- `docs/architecture/ORUN_TLP_V2_PRODUCT_SECURE_WIRE.md`
- `docs/architecture/ORUN_TLP_V2_TRACKER_PRODUCT_DATA_CONTRACT.md`

## 1. Goal

Implement the smallest portable fixed-slot tracker ObservationStore over the
existing abstract `FlashBackend` without choosing physical nRF addresses yet.

The store owns durable tracker product facts:

```text
PERIODIC_OBSERVATION
EVENT
COMMAND_RESULT
```

It does not own PRODUCT_SECURE crypto, LoRa scheduling, Gateway custody, Edge
transport or the future 128 KiB physical partition.

## 2. Required invariants

SF5C must prove on host/fault tests:

- deterministic bounded boot recovery;
- body/CRC then commit-last record admission;
- no already-committed record is rewritten in place;
- one tracker-wide record sequence within one non-zero incarnation;
- PERIODIC / EVENT / RESULT share that record-identity namespace;
- finite-capacity rotation remains oldest-first;
- overwritten unreleased history increments bounded capacity-loss diagnostics;
- release state survives reset and safely represents bounded non-prefix ACKs;
- reset/checkpoint loss cannot make the complete backlog re-protect forever;
- at most four exact custody-request objects need durable exact-byte ownership
  at once, across PERIODIC/EVENT/RESULT;
- an exact custody object becomes durable before first RF transmission in the
  later runtime owner;
- open EVENT occurrence control state survives reset and historical record
  rotation until CLEARED/reconciled;
- retained logical RESULT metadata can distinguish the canonical request tuple
  required by SF5B without creating one durable row per crypto retry;
- committed corruption/read failure fails closed where it could otherwise cause
  premature release or silent record loss;
- asynchronous `FlashBackend::kPending` never causes resubmission of the same
  physical mutation;
- `hasUnreconciledMutation()` at begin fails closed.

## 3. Format direction

Keep the first implementation intentionally small:

- fixed 4096-byte logical pages;
- fixed-size record slots;
- self-describing committed page header with version/inverse/CRC;
- record type + logical record identity + bounded payload;
- record CRC + commit-last seal;
- bounded page-generation ordering;
- bounded control metadata for release/open-EVENT/RESULT/exact-object state;
- no heap allocation;
- bounded RAM proportional to page count or a small fixed product cap;
- no variable-length allocator/fragmentation layer.

The exact slot/control byte geometry is frozen only when tests demonstrate it
satisfies the required power-cut/recovery behavior. Do not optimize field bytes
at the cost of semantic correctness.

## 3.1 Implemented portable geometry

The current SF5C implementation now freezes the **portable format geometry**
subject to host/fault validation and independent audit:

- 4096-byte logical pages;
- 64-byte self-describing page header;
- 96-byte data record;
- 42 data records per data page;
- 144-byte control record;
- 28 control records per control page;
- two logical control pages;
- schema-v1 canonical data payload sizes:
  - PERIODIC = 64 bytes;
  - EVENT = 48 bytes;
  - RESULT = 32 bytes;
- two per-record release marker words outside the record CRC;
- commit-last body/CRC -> commit admission;
- activation-last page ownership.

For the owner-approved future 128 KiB physical target, this geometry gives the
following **planning arithmetic only**:

```text
128 KiB / 4 KiB = 32 pages
2 control pages
30 data pages
30 * 42 = 1260 data-record slots
```

If all 1260 slots were PERIODIC records, approximate retention would be:

```text
3 min   =  2.625 days
5 min   =  4.375 days
15 min  = 13.125 days
20 min  = 17.5 days
30 min  = 26.25 days
60 min  = 52.5 days
```

EVENT and RESULT records consume the same data-slot pool and reduce those
figures. This arithmetic is **not** a physical endurance/power/layout claim;
SF5D still owns the actual nRF address reservation and update/DFU gate.

## 3.2 Responsibility release and full-store recovery

Each committed data slot has two independent release-marker words outside the
immutable record CRC. A successful authenticated custody release programs one
marker. A torn first marker remains fail-closed and the second marker permits
one bounded retry without rewriting the record.

If both marker writes are torn, SF5C does **not** guess that responsibility was
transferred. The record remains retained and is surfaced as
`release_uncertain + release_marker_exhausted`. Later runtime must treat this
as an explicit storage/reconciliation condition rather than silently releasing
the record or spinning unbounded RF retries.

When no erased data page remains and the active page is full, maintenance:

1. reclaims a fully released page first when one exists;
2. otherwise selects the oldest active-generation page;
3. durably appends a StoreState rotation intent **before erase**;
4. erases/re-prepares that exact page with a newer generation;
5. durably appends rotation-complete StoreState.

Only unreleased records on the retired page increment cumulative capacity-loss
diagnostics. Power loss after the intent is recoverable because the intent names
the exact page and old/new generations allowed to be in transition.

## 3.3 Open EVENT ordering contract

Open-occurrence control state is independent of the historical EVENT row, so
ring rotation cannot silently mint a second occurrence identity.

The later SF5F runtime must use this ordering:

```text
ACTIVE transition:
  durable open-occurrence control state
  -> durable ACTIVE EVENT record
  -> RF eligibility

CLEARED transition:
  durable CLEARED EVENT record using the same occurrence_id
  -> durable clear/tombstone of open-occurrence control state
```

A reset between the two CLEARED steps may leave a safe duplicate/reconciliation
case, but must never mint a new occurrence ID while the durable open state still
exists. If open-occurrence authority is uncertain, fail closed/reconcile.

For BATTERY_STATE, measured battery millivolts are transition context, not the
open-occurrence key; voltage drift while one LOW/CRITICAL condition remains
active does not create another occurrence. SUBSYSTEM_FAULT includes the bounded
subsystem identifier in its occurrence key.

## 3.4 RESULT guard ordering

RESULT persistence is guard-first. For one authenticated command authority
context + `command_id`, SF5C durably records the canonical request tuple and
reserves the next record identity before a RESULT row may commit.

While that guard exists without its RESULT row:

- PERIODIC/EVENT cannot consume the reserved sequence;
- reset recovers the same reservation;
- only the matching RESULT may consume it.

Same authority + `command_id` with a different canonical tuple fails closed
while the retained logical RESULT/guard is live. Once the RESULT ages out of
the bounded tracker history, old command validity remains governed by the
separate delegated replay/CAS owners; SF5C does not become an unbounded command
journal.

## 4. Deliberate non-goals

SF5C does not:

- allocate `0x0C5000..0x0E5000`;
- change linker/application ceiling;
- add `NrfObservationFlash`;
- change TLP v1 or SF2/SF3 fixtures;
- activate PRODUCT_SECURE RF;
- enable custody ACK runtime;
- implement EDGE_DURABLE_ACCEPT;
- implement BLE/LoRa DFU or LoRa FOTA;
- claim physical wear/power evidence.

Those belong to SF5D+.

## 5. Validation gate

Required before merge:

```text
focused ObservationStore host tests
-> deterministic torn-write/power-cut fault matrix
-> async FlashBackend pending/reconcile tests
-> full existing host regression
-> ASan/UBSan + -Wall -Wextra -Werror
```

A RAK4630 production build is useful as an integration/size guard if the portable
headers become reachable from target compilation, but SF5C still does not claim
physical flash behavior.

Independent audit is required after the implementation/test head is stable and
before merge because this slice defines reset/release/persistence behavior that
later controls data-loss semantics.
