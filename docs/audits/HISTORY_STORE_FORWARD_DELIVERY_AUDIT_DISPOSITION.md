# History Store-Forward Delivery — Independent Audit Disposition

Status: **PASS WITH FIXES RECEIVED; FIXES APPLIED; FINAL VERIFICATION PENDING.**

PR: #69

Branch: `design/history-store-forward-delivery-contract`.

Current canonical target for final verification must be read from the PR head;
do not reuse the historical audit verdict as evidence for later commits.

## 1. Evidence boundary

The independent reviewer audited the pre-hardening M4P1 contract and the
repository code it depends on, including HistoryStore, storage/journal format,
SecurityStore/security format, the M7P6 security architecture, delegated-command
security direction and downlink rendezvous plan.

Recovered verdict:

```text
PASS WITH FIXES
BLOCKER 0
HIGH    2
MEDIUM  4
LOW     5
```

The recovered summary did not print the exact audited head SHA. Do not invent
one. The verdict is therefore retained as historical evidence for the
pre-hardening PR state only.

## 2. HIGH findings

### H1 — BACKEND_DURABLE authority ambiguity

Risk: if a delegated gateway key could authenticate the receipt that advances
History delivery state, a compromised or faulty gateway could claim backend
durability without any backend commit.

Disposition:

- BACKEND_DURABLE is now explicitly a backend-authority A2D statement rooted in
  the device credential;
- delegated gateway grant/frame material cannot authorize
  `delivered_through`;
- gateway may transport opaque receipt bytes but cannot mint the authority fact;
- validation requires delegated-key forged receipt rejection with zero History
  mutation.

Status: **FIX APPLIED; FINAL VERIFICATION PENDING.**

### H2 — stop-and-wait / rendezvous drain-rate risk

Risk: one receipt per record may not drain faster than new data production once
10-second RX rendezvous timing, relay delay, Internet/backend latency, gateway
half-duplex downlink airtime, retries and continuing live traffic are included.

Disposition:

- one-distinct-outstanding stop-and-wait is no longer frozen as the only
  production receipt shape;
- safety invariants remain explicit-identity receipt + contiguous durable
  watermark;
- bounded explicit-identity batch receipt is allowed if required;
- SF2 wire freeze is blocked on candidate backlog-drain feasibility;
- the model is rerun with exact frame lengths before SF3 activation.

Status: **FIX APPLIED; FINAL VERIFICATION PENDING.**

## 3. MEDIUM findings

### M1 — metadata-only History rotation

Disposition: checkpoint must never initiate page rotation/erase solely because
four state slots are exhausted; durability defers or format is revised.
Validation requires append-free receipt/checkpoint sequences to produce zero
History erases.

Status: **FIX APPLIED.**

### M2 — SecurityStore wear omitted

Disposition: A2D replay-reservation and D2A TX-reservation writes/erases are now
part of the store-forward wear budget. Current engineering constants are
recorded, without turning them into permanent product constants.

Status: **FIX APPLIED.**

### M3 — incarnation source could reset with History

Disposition: a monotonic incarnation stored only inside the destructively
erased History region is invalid. SF1 must use either approved CSPRNG-derived
incarnation committed before the first new record or monotonic durability outside
the History erase/re-baseline region.

Status: **FIX APPLIED.**

### M4 — live vs backlog receipt relationship undefined

Disposition: live POSITION remains non-blocking; explicit authenticated backend
delivery facts for stored live records may enter the same bounded RAM
acknowledged-ID set. Durable progress remains contiguous and reboot may cause
safe replay.

Status: **FIX APPLIED.**

## 4. LOW findings

- **L1 baseline stale** — corrected to current M7P6H main baseline.
- **L2 persistent replay cursor unsafe** — excluded from production SF3 replay
  semantics; legacy state must be ignored/re-baselined or replaced.
- **L3 outstanding record overwritten** — treated as capacity loss; late receipt
  cannot mutate delivery state for a missing record.
- **L4 delayed receipt freshness ambiguity** — durable fact is not invalidated by
  wall-clock delay; transport anti-replay stays in A2D security/counter layer.
- **L5 authenticated downstream contact undefined** — backend-authority A2D
  contact defined; no-contact replay is bounded by one probe per backoff
  interval.

Status: **FIXES APPLIED.**

## 5. Remaining gate

This file is a disposition summary, not an independent final verification.

Before M4P1 merge:

- reviewer must inspect the post-fix PR head;
- no BLOCKER/HIGH may remain;
- documentation must still be the only diff;
- no build/physical PASS is required for this documentation-only slice.

SF1 runtime/storage implementation begins only after that gate closes.
