# SF5C — Portable ObservationStore

Status: **ACTIVE IMPLEMENTATION SLICE — PORTABLE STORAGE ONLY; NO nRF ADDRESS
ALLOCATION, NO PRODUCTION RF CUTOVER, NO PHYSICAL PASS.**

Baseline:
`main@5b757966489e5fe221c05fe1cbf66ff7b1e602d5` (SF5B merged).

Branch:
`feat/sf5c-observation-store`.

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
