# M4P5A — SF3 History delivery RAM coordinator

Status: **IMPLEMENTATION IN PROGRESS — NO PRODUCTION RF/RUNTIME ACTIVATION**

Baseline:

```text
main@01197bdfaa59c27f12846ffdfb0f95cb6ce084cf
```

Branch:

```text
feat/m4p5a-history-delivery-coordinator
```

## 1. Why this slice exists

M4P1–M4P4 froze the History/store-forward safety contract, safe History
delivery watermark foundation, History incarnation and exact TLP v2
`HISTORY_SECURE` wire. M7P6I then closed the missing production
root-credential crypto seam.

SF3 still needs a runtime owner for the application-level delivery facts that
arrive after secure receipt authentication and A2D anti-replay admission.

This first SF3 slice implements only that RAM owner. It deliberately does not
enable History RF replay yet.

## 2. Product invariant

Only a backend-authority receipt which has already passed both:

1. M7P6I BACKEND_A2D AEAD authentication; and
2. SecurityStore A2D replay admission

may reach this coordinator.

The coordinator then applies explicit History record identities without
confusing:

```text
History identity
!= numeric sequence/ticket gap
!= secure counter
!= TX_DONE
!= gateway RF receipt
```

## 3. Scope

Add `HistoryDeliveryCoordinator` with a fixed RAM selective-acknowledgement
set.

Initial bound:

```text
2 * M4P4 max receipt identities
= 2 * 6
= 12 identities
= 96 bytes identity storage
```

Behavior:

- accepts only M4P4-shaped bounded explicit identity lists;
- every new identity must still be an actual retained History record;
- newer explicit identities may wait in RAM;
- only the oldest contiguous prefix of actual History records may advance
  `HistoryStore::acknowledgedThrough()`;
- numeric ticket gaps are never inferred as delivered observations;
- duplicate already-acknowledged facts are idempotent;
- an unknown/overwritten identity rejects the entire receipt application before
  History RAM mutation;
- selective-set overflow rejects before History RAM mutation;
- stale selective facts whose records have already been capacity-overwritten
  may be pruned because they can no longer cause deletion or delivery progress.

## 4. Explicit non-scope

This slice does **not**:

- call `HistorySecureCrypto`;
- call or mutate `SecurityStore`;
- receive or send LoRa packets;
- change `RadioManager`;
- enable relay forwarding for `HISTORY_SECURE`;
- change TLP v1 or M4P4 wire bytes;
- write a durable History delivery checkpoint;
- persist a replay cursor;
- choose retry/backoff/contact timers;
- add gateway/backend/mobile runtime;
- claim secure-RF, outage recovery or power-cut PASS.

The method name and contract explicitly require the caller to invoke it only
after SecurityStore returned accepted=true for the exact authenticated A2D
receipt lifetime.

## 5. Why checkpointing is not in M4P5A

History delivery checkpoint writes are flash-wear sensitive. The current safe
HistoryStore API correctly refuses metadata-only page rotation, but choosing
*when* to consume one of the bounded state slots is an SF3 scheduling/wear
policy decision.

M4P5A therefore advances only the existing RAM
`acknowledged_through` watermark. A later SF3 slice will freeze and test the
coarse durable checkpoint policy.

Duplicate replay after reboot remains preferable to premature deletion.

## 6. Host verification

Dedicated host coverage must lock at least:

- out-of-order explicit receipt IDs stay selective until the real oldest gap
  closes;
- numeric ticket gaps do not require fake acknowledgements;
- unknown identity rejects atomically;
- the 12-identity RAM set is bounded and overflow is atomic;
- duplicate already-contiguous facts are idempotent;
- invalid/busy History paths do not mutate RAM delivery progress.

No hardware qualification is required for this RAM-only, transport-neutral
slice unless later code changes touch production runtime, flash mutation,
radio, security crypto or hardware drivers.


## 7. Focused host validation

The owner ran the dedicated M4P5A host test with strict warnings and
ASan/UBSan:

```text
g++ -std=c++17 -O1 -g -Wall -Wextra -Werror \
  -fsanitize=address,undefined -fno-sanitize-recover=undefined \
  -Ifirmware/include \
  firmware/tests/m4/test_m4p5a_history_delivery_coordinator.cpp \
  firmware/src/history_delivery_coordinator.cpp \
  firmware/src/history_store.cpp \
  firmware/src/journal_format.cpp \
  firmware/src/tlp_position_packet.cpp \
  -o /tmp/m4p5a && /tmp/m4p5a
```

Result: **PASS** (silent exit to shell prompt).

This focused gate covers the RAM-only coordinator semantics and does not
constitute RF, flash power-cut, gateway/backend or physical-hardware evidence.

Remaining PR-completion gates:

- aggregate host regression;
- RAK4630 production build, because the new source file is part of the firmware
  source tree even though no production runtime caller exists yet;
- focused review/audit before merge.


## 8. Aggregate host regression

The owner ran the full aggregate host suite:

```text
bash firmware/tests/run_host_tests.sh
```

Result: **PASS**.

The completed output included, among others:

- production startup scenarios PASS;
- R2/R4 radio ownership/recovery guards PASS;
- B4 relay behavior PASS;
- M6P1 listen-window checks PASS;
- M4P4 History secure D2A/A2D vectors PASS;
- M7P6I SecurityStore-owned D2A vector PASS;
- BLE/storage/tooling source-contract guards PASS.

This is host regression evidence only. It does not constitute physical RF,
flash power-cut, gateway/backend or hardware qualification.

Remaining PR-completion gates:

- RAK4630 production build;
- focused independent review/audit.


## 9. RAK4630 production build

The owner built the normal production image:

```text
pio run -d firmware -e rak4630

RAM:   29,024 / 248,832 bytes (11.7%)
Flash: 265,356 / 815,104 bytes (32.6%)
SUCCESS — 79.65 s
```

Result: **PASS**.

The normal production footprint is unchanged from the M7P6I baseline:

```text
RAM:   29,024 bytes
Flash: 265,356 bytes
```

This confirms M4P5A adds no active production runtime caller. The new
HistoryDeliveryCoordinator remains transport-neutral and dormant in the normal
firmware image.

Remaining merge gate:

- independent focused review/audit of the exact final branch head.


## 10. Independent audit and post-audit fixes

Independent audit of exact head:

```text
82ffae948abf6bd9ab870c68c882226baa94f8ea
```

returned:

```text
VERDICT: PASS WITH FIXES
BLOCKER 0
HIGH 0
MEDIUM 3
LOW 3
```

Merge-blocking findings:

- **M1:** a full selective set could deadlock after capacity overwrite when an
  already-selective duplicate became the oldest surviving actual record;
- **M2:** commit-phase `kInvariantFailure` could expose a shorter safe RAM
  prefix despite the original documentation claiming full atomicity.

Non-blocking but pre-runtime **M3** also identified that authenticated History
incarnation was not explicit at the coordinator seam.

Post-audit fixes on this branch:

- drain already-authenticated selective prefix immediately after stale pruning;
- expose/use strict fail-closed retained-record traversal during delivery
  preflight and commit;
- document `kInvariantFailure` precisely: it may leave a strictly validated
  safe RAM prefix, which is never rolled back;
- bind each coordinator application to the exact authenticated History
  incarnation and isolate selective RAM across a destructive re-baseline;
- add overwrite, no-flash-mutation, transient-read-fault, incarnation-reuse and
  receipt-validation edge tests.

The detailed disposition is recorded in
`docs/audits/M4P5A_HISTORY_DELIVERY_COORDINATOR_AUDIT_DISPOSITION.md`.

Because production source/header code changed after the audit, the earlier host
and RAK build evidence remains historical. Exact post-fix validation is required
before final independent verification.


## 11. Post-audit focused host validation

After applying the independent-audit M1/M2 fixes and the M3 incarnation seam
hardening, the owner reran the dedicated M4P5A host test with strict warnings
and ASan/UBSan:

```text
g++ -std=c++17 -O1 -g -Wall -Wextra -Werror \
  -fsanitize=address,undefined -fno-sanitize-recover=undefined \
  -Ifirmware/include \
  firmware/tests/m4/test_m4p5a_history_delivery_coordinator.cpp \
  firmware/src/history_delivery_coordinator.cpp \
  firmware/src/history_store.cpp \
  firmware/src/journal_format.cpp \
  firmware/src/tlp_position_packet.cpp \
  -o /tmp/m4p5a && /tmp/m4p5a
```

Result: **PASS** (silent exit to shell prompt).

This exact post-fix focused gate includes coverage for:

- full selective-set drain after capacity overwrite;
- strict retained-record traversal;
- transient commit read fault / safe-prefix invariant behavior;
- no durable History checkpoint or flash-byte mutation;
- History incarnation re-baseline isolation;
- receipt validation edge cases.

The earlier RAK4630 build predates these production-source/header changes and is
therefore retained only as historical evidence.

Remaining post-fix gates:

- aggregate host regression;
- fresh RAK4630 production build;
- independent focused final verification.


## 12. Post-audit aggregate host regression

After the M1/M2 fixes and M3 incarnation binding, the owner reran:

```text
bash firmware/tests/run_host_tests.sh
```

Result: **PASS**.

The completed output included PASS for the existing History secure D2A/A2D
vectors, M7P6I SecurityStore-owned D2A vector, BLE framework/admission/GATT
guards, R4 I2C/watchdog guards and DEVQ1 tooling/CodeQL guards.

This is host regression evidence only.

Remaining post-fix owner-side gate:

- fresh RAK4630 production build.

After that, independent focused final verification remains before merge.
