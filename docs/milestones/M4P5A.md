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
