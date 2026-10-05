# M4P5B authenticated History receipt admission — audit disposition

Independent focused audit target:

```text
PR #76
baseline 0f6980e93ddcc83bf2aee789772e34605f8846c7
audited head a31b0a383978b11beb99cdef458868a687d58449
```

Independent verdict: **PASS WITH FIXES**

Findings:

- BLOCKER: 0
- HIGH: 0
- MEDIUM: 1
- LOW: 4

## M1 — packet/plaintext/credential binding existed only by caller convention

Status: **FIXED — final verification pending**

The audited API accepted three independently supplied values:

```text
HistorySecurePacket
BackendDurableReceiptPlaintext
authenticated credential_id
```

That allowed a future incorrect runtime caller to pair the fresh
counter/incarnation/credential from authenticated frame B with plaintext from
authenticated frame A when both plaintexts had the same encoded length.

The independent probe demonstrated the security consequence across a destructive
History re-baseline: local History identities restarted under a new incarnation,
and mixed packet-B/plaintext-A input could acknowledge a new-incarnation record
which the backend had never durably accepted in that incarnation.

The post-audit fix introduces
`AuthenticatedBackendDurableReceipt` in `history_secure_crypto.h`.

Production properties:

- callers can create only an empty output destination;
- packet/plaintext/credential fields are private;
- `HistorySecureCrypto` is the production writer after one successful AEAD
  open;
- `HistoryReceiptAdmissionCoordinator` is the application consumer;
- M4P5B no longer exposes a submit overload taking separate packet/plaintext/
  credential arguments.

A test-only friend exists only under `ORUN_M4P5B_HOST_TEST` so host tests can
construct negative fixtures without exposing a production forging API.

The existing lower-level M7P6I open overload remains for its already-validated
callers/tests, but raw outputs from that overload cannot be submitted into
M4P5B. A preferred overload now directly produces the opaque authenticated
object while preserving the existing proven crypto implementation.

## L1 — kDeliveryInvariantFailure caller reminder

Status: **FIXED — final verification pending**

The M4P5B public service contract now states that
`kDeliveryInvariantFailure` may follow a shorter strict-validated M4P5A RAM
prefix. The future caller must re-read `acknowledgedThrough()`; no rollback is
allowed and the consumed A2D counter must not be retried.

A focused fault-injection regression now locks the terminal result, safe-prefix
behavior, same-counter rejection and fresh-counter recovery path.

## L2 — no explicit cancellation for indefinitely unavailable History

Status: **ACCEPTED / DEFERRED TO RUNTIME WIRING**

If replay is accepted and History remains permanently unavailable, M4P5B keeps
the receipt in `kAwaitDelivery` and blocks another submit.

This is fail-safe: no premature History acknowledgement is possible and no
second security counter is consumed.

M4P5B deliberately has no production caller or timeout policy. The first
runtime-wiring slice must define how a persistent History fault tears down or
resets this pending receive owner rather than adding a speculative cancellation
API here.

## L3 — reboot test does not reconstruct HistoryStore

Status: **DOCUMENTED**

The reboot regression reconstructs SecurityStore, which is the owner whose
durable replay state is under test. HistoryStore remains the same object, but at
the simulated cut point both its RAM acknowledgement watermark and selective set
are still zero, so the security replay-boundary outcome is equivalent.

The test comment now states this explicitly. It is not physical power-cut
evidence.

## L4 — missing edge regressions

Status: **FIXED — final verification pending**

Permanent host regressions were added for:

- History incarnation change after SecurityStore replay acceptance but before
  M4P5A application;
- selective acknowledgement set full after replay acceptance;
- M4P5A `kInvariantFailure` after replay acceptance, including shorter safe
  RAM prefixes, consumed-counter rejection and fresh-counter recovery.

The original malformed-object test remains as test-only defense-in-depth using
the host-only capability friend.

## Scope remains unchanged

The post-audit changes do not:

- activate a production caller;
- receive or send LoRa;
- change RadioManager;
- enable HISTORY_SECURE relay forwarding;
- add sender retry/backoff policy;
- write a History delivery checkpoint;
- persist replay cursor state;
- change SecurityStore or History flash formats;
- change TLP v1 or frozen M4P4 wire bytes;
- add gateway/backend/mobile runtime.

Because production security-seam code and tests changed after the original
audit, the pre-fix host/build evidence remains historical. Focused host,
aggregate host and RAK4630 production build must be rerun before independent
final verification.
