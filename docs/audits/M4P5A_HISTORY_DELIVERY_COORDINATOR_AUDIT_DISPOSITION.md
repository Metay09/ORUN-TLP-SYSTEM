# M4P5A History delivery RAM coordinator — independent audit disposition

Independent audit target:

```text
PR #75
baseline 01197bdfaa59c27f12846ffdfb0f95cb6ce084cf
audited head 82ffae948abf6bd9ab870c68c882226baa94f8ea
```

Independent verdict: **PASS WITH FIXES**

Findings:

- BLOCKER: 0
- HIGH: 0
- MEDIUM: 3
- LOW: 3

## M1 — full selective set could deadlock after capacity overwrite

Status: **FIXED — final verification pending**

The audited implementation drained the selective set only after accepting a new
receipt identity. If the original missing oldest record was capacity-overwritten,
an identity already present in a full selective set could become the new oldest
surviving record. A duplicate receipt for that identity skipped the drain loop,
while every newer identity was rejected as set-full.

The fix runs strict contiguous selective-prefix draining after stale pruning and
before processing the new receipt. A full set can therefore unlock when its
already-authenticated first member becomes the oldest surviving actual record.

No lost/overwritten record is reclassified as delivered. History capacity-loss
diagnostics remain the owner of that loss fact.

## M2 — commit-phase invariant failure could expose partial safe RAM progress

Status: **FIXED CONTRACT / HARDENED PREFLIGHT — final verification pending**

The audited preflight used permissive `readAfter()/lookup()`, while the final
HistoryStore commit used fail-closed `readNextRetainedStrict()`. A persistent
read/decode fault could therefore pass preflight and fail only after part of the
strictly validated RAM prefix had already advanced.

The fix exposes a read-only `HistoryStore::readNextRetained()` wrapper over the
existing strict traversal and uses it throughout delivery-sensitive preflight
and commit navigation.

Persistent slot/read inconsistency is therefore rejected before History RAM
mutation whenever it is observable in preflight.

A transient read fault can still occur after successful preflight. Rolling back
`acknowledged_through` is intentionally forbidden. The API contract now states
that `kInvariantFailure` may leave a shorter, individually strict-validated safe
RAM prefix committed. Callers must re-read `acknowledgedThrough()` after that
result. Ordinary invalid/unknown/full/unavailable rejection paths remain atomic.

No durable History checkpoint is written by this behavior.

## M3 — authenticated History incarnation was not carried into the coordinator

Status: **FIXED — final verification pending**

The audited API accepted only receipt plaintext. A future deferred SF3 wiring
could therefore lose the authenticated History incarnation between AEAD open,
SecurityStore replay admission and application delivery.

The coordinator now requires the exact authenticated History incarnation from
the successfully opened receipt packet and rejects zero/mismatched values.

The selective RAM set is incarnation-bound. If History is explicitly
re-baselined and local identities restart, old selective facts are ignored; the
new binding is committed only with a successfully applied receipt for the
current incarnation.

This is a small seam hardening before any production SF3 caller exists.

## L1 — capacity-loss diagnostic is conservative

Status: **ACCEPTED**

`capacity_lost_undelivered` may count a record which had a backend-durable fact
waiting only in the coordinator's selective RAM set. The diagnostic is
conservative and does not cause deletion or delivery advancement.

## L2 — result/diagnostic nuances

Status: **ACCEPTED**

Stale-prune-only state cleanup may still accompany a duplicate result, and
rejected scratch work does not publish stale-prune diagnostics. These are
observability details, not safety violations.

## L3 — cheap receipt-validation coverage gaps

Status: **FIXED — final verification pending**

The dedicated test now also covers:

- count greater than the frozen six-identity maximum;
- zero identity;
- repeated/non-increasing identity;
- unavailable HistoryStore;
- mismatched authenticated History incarnation.

## Added regression coverage

Post-audit tests add explicit coverage for:

- full 12-ID selective set after capacity overwrite, including duplicate oldest
  receipt draining;
- no flash-byte mutation and unchanged durable `deliveredThrough`;
- transient read fault during the preflight/commit window, including the
  documented safe-prefix `kInvariantFailure` behavior;
- destructive History re-baseline with local identity reuse under a new
  incarnation, proving old selective facts cannot acknowledge the new stream.

## Scope remains unchanged

The fixes do not:

- add a production caller;
- call or mutate SecurityStore;
- call History delivery checkpoint APIs;
- persist replay cursor state;
- change RadioManager or relay allow-lists;
- change TLP v1 or frozen M4P4 wire bytes;
- activate secure History RF;
- add gateway/backend/mobile runtime.

Because production-source and public-header code changed after the first audit,
the pre-fix PASS evidence remains historical. Focused host, aggregate host and
RAK4630 build must be rerun before focused final verification.
