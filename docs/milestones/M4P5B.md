# M4P5B — SF3 authenticated History receipt admission ordering

Status: **IMPLEMENTATION IN PROGRESS — NO PRODUCTION RF/RUNTIME ACTIVATION**

Baseline:

```text
main@0f6980e93ddcc83bf2aee789772e34605f8846c7
```

Branch:

```text
feat/m4p5b-history-receipt-admission
```

## 1. Why this slice exists

M7P6I authenticates/decrypts the frozen BACKEND_A2D History receipt but
intentionally does not mutate replay state.

M4P5A applies explicit BACKEND_DURABLE identities to the bounded History RAM
delivery coordinator, but intentionally starts only after replay admission.

The missing safety seam is the exact ordering between those two owners:

```text
HistorySecureCrypto AEAD success
 -> exact authenticated credential lifetime
 -> SecurityStore A2D replay admission
 -> accepted == true
 -> M4P5A History delivery application
```

M4P5B closes only that ordering seam. It does not enable RF receive.

## 2. Non-negotiable ordering

A future runtime caller must obtain one
`AuthenticatedBackendDurableReceipt` from a successful
`HistorySecureCrypto::openBackendDurableReceipt()` call.

That object keeps the authenticated packet, plaintext receipt and credential
snapshot inseparable at the M4P5B API boundary. M4P5B then submits exactly:

```text
authenticated_credential_id
packet.key_epoch
packet.security_counter
```

to `SecurityStore::submitAuthenticatedA2dCounter()`.

It must never re-read `currentCredentialId()` for an already-authenticated
receipt, and it no longer accepts separate packet/plaintext/credential
arguments that a future caller could mix across two successful opens.

History delivery application is forbidden until
`takeA2dReplayResult()` returns `accepted=true`.

## 3. Async replay reservation behavior

SecurityStore may need a durable A2D replay-reservation write before it can
publish acceptance.

M4P5B therefore owns one bounded pending receipt in RAM:

```text
IDLE
 -> AWAIT_REPLAY
 -> AWAIT_DELIVERY
 -> terminal
```

`SecurityStore::poll()` remains externally owned by the composition root.
M4P5B never polls flash itself.

If replay was accepted but History is temporarily busy, M4P5B retains the
already-admitted receipt in RAM and retries only the M4P5A application step. It
must not consume another A2D security counter merely because HistoryStore was
busy.

If M4P5A returns `kInvariantFailure`, M4P5B treats the receipt as terminal.
A shorter strict-validated RAM prefix may already have advanced, so the future
caller must re-read `acknowledgedThrough()`. The consumed A2D counter is never
retried; backend recovery uses the same logical receipt fact under a fresh
counter.

If History remains permanently unavailable after replay acceptance, this slice
intentionally keeps the receipt pending and blocks later submits. That is
fail-safe. The first production runtime-wiring slice must define persistent
History-fault teardown/reset ownership; M4P5B does not invent a timeout or
cancellation policy without a caller.

While a receipt is pending, M4P5B is the sole consumer of
`SecurityStore::takeA2dReplayResult()`. There is currently no production A2D
dispatcher/caller, so this introduces no ownership conflict. Before another
protected A2D application family is activated, replay-result consumption must
be serialized through one reviewed receive owner; two independent consumers
must never race on SecurityStore's single result channel.

## 4. Power-loss boundary

There is an unavoidable safe window:

```text
SecurityStore replay accepted/durable
 -> power loss
 -> History RAM delivery effect never happened
```

After reboot the same A2D counter is correctly rejected by SecurityStore.

The backend must therefore reissue the same logical BACKEND_DURABLE History
fact under a fresh A2D security counter. The History observation identity does
not change.

This yields duplicate/retry cost, not premature deletion.

## 5. Input seam hardening

M4P5B does not authenticate cryptography again.

After the independent audit, packet/plaintext/credential binding is no longer a
caller convention. `HistorySecureCrypto` is the production writer of one
opaque `AuthenticatedBackendDurableReceipt`, and M4P5B consumes only that
object.

The coordinator still performs structural cross-checks on the private contents:

- packet must be valid frozen `HISTORY_SECURE`;
- context must be `BACKEND_A2D`;
- family must be `BACKEND_DURABLE receipt`;
- packet ciphertext length must match the bound receipt count;
- bound receipt must satisfy the frozen explicit-ID plaintext rules.

These checks are defense-in-depth after AEAD success, not a second authority.
The production API does not expose a way to rebuild or remix the private tuple.

## 6. Explicit non-scope

M4P5B does **not**:

- call `HistorySecureCrypto` itself;
- receive LoRa frames;
- change `RadioManager`;
- enable `HISTORY_SECURE` RF;
- enable relay forwarding of `HISTORY_SECURE`;
- select/send historical observations;
- choose retry/backoff/contact timers;
- write a durable History delivery checkpoint;
- persist replay cursors;
- add gateway/backend/mobile runtime;
- change TLP v1 or frozen M4P4 bytes;
- change SecurityStore or History flash formats.

No normal production caller is added by this slice.

## 7. Required host coverage

The focused host test must use the real `SecurityStore`,
`HistoryDeliveryCoordinator` and `HistoryStore` owners with synchronous fault
backends and lock at least:

- no History acknowledgement before replay acceptance;
- async SecurityStore reservation -> accepted -> delivery application;
- replay duplicate/old counter rejects without History mutation;
- wrong authenticated credential lifetime rejects before History delivery;
- one pending receipt blocks a second submission;
- replay accepted + temporarily busy History retains the receipt and later
  applies it without another security counter;
- malformed packet/plaintext pairing rejects before replay mutation;
- reboot after replay commit but before History effect requires a fresh A2D
  counter and still cannot cause premature History delivery.

This is host/state-machine evidence only. It is not physical RF, flash
power-cut, backend or outage-recovery qualification.


## 8. Focused host validation

The owner ran the dedicated M4P5B host test with strict warnings and
ASan/UBSan:

```text
g++ -std=c++17 -O1 -g -Wall -Wextra -Werror \
  -fsanitize=address,undefined -fno-sanitize-recover=undefined \
  -Ifirmware/include \
  firmware/tests/m4/test_m4p5b_history_receipt_admission.cpp \
  firmware/src/history_receipt_admission.cpp \
  firmware/src/history_delivery_coordinator.cpp \
  firmware/src/history_store.cpp \
  firmware/src/security_store.cpp \
  firmware/src/security_format.cpp \
  firmware/src/journal_format.cpp \
  firmware/src/tlp_position_packet.cpp \
  firmware/src/tlp_v2_history_secure.cpp \
  -o /tmp/m4p5b && /tmp/m4p5b
```

Result: **PASS** (silent exit to shell prompt).

This focused gate covers only the transport-neutral receive/admission state
machine. It does not constitute physical RF, backend, outage-recovery,
power-cut or brownout evidence.

Remaining PR-completion gates:

- aggregate host regression;
- normal RAK4630 production build;
- independent focused review/audit.


## 9. Aggregate host regression

The owner ran the complete host suite:

```text
bash firmware/tests/run_host_tests.sh
```

Result: **PASS**.

The run completed with the new M4P5B host gate integrated into the aggregate
runner and preserved PASS across the existing:

- History/SecurityStore regression suites;
- M4P4 secure History vectors;
- M7P6I SecurityStore-owned D2A vector;
- production startup scenarios;
- radio ownership/listen-window guards;
- BLE/storage/tooling guards.

This remains host evidence only and does not constitute physical RF,
backend/outage-recovery, power-cut or brownout qualification.

Remaining PR-completion gates:

- normal RAK4630 production build;
- independent focused review/audit.


## 10. RAK4630 production build

The owner built the normal production image:

```text
pio run -d firmware -e rak4630

RAM:   29,024 / 248,832 bytes (11.7%)
Flash: 265,356 / 815,104 bytes (32.6%)
SUCCESS — 79.39 s
```

Result: **PASS**.

The normal production footprint remains unchanged from the M4P5A/M7P6I
baseline:

```text
RAM:   29,024 bytes
Flash: 265,356 bytes
```

This is consistent with M4P5B adding no active production runtime caller.

Owner-side validation gates now complete:

- focused strict-warning ASan/UBSan host test: PASS;
- aggregate host regression: PASS;
- RAK4630 production build: PASS.

Remaining merge gate:

- independent focused review/audit of the exact branch head.


## 11. Independent audit and post-audit fixes

Independent focused audit of exact head:

```text
a31b0a383978b11beb99cdef458868a687d58449
```

returned:

```text
VERDICT: PASS WITH FIXES
BLOCKER 0
HIGH 0
MEDIUM 1
LOW 4
```

The medium finding demonstrated that the original three-argument API could be
misused by a future caller to combine packet/counter/incarnation from one
authenticated frame with plaintext from another. Across History re-baseline and
identity reuse, that could become premature acknowledgement.

Post-audit changes:

- add opaque `AuthenticatedBackendDurableReceipt`;
- add a preferred HistorySecureCrypto overload which produces that object only
  after one successful AEAD open;
- make M4P5B accept only the opaque object;
- keep private tuple fields inaccessible in production;
- correct the defense-in-depth comments;
- document `kDeliveryInvariantFailure` safe-prefix behavior;
- document persistent-History-unavailable liveness as a future runtime-owner
  decision;
- clarify the reboot host-model boundary;
- add incarnation-change, selective-set-full and invariant-failure recovery
  regressions.

Detailed disposition:

`docs/audits/M4P5B_HISTORY_RECEIPT_ADMISSION_AUDIT_DISPOSITION.md`.

Because production security-seam code changed after the audit, the earlier
focused/aggregate/build evidence is historical. Exact post-fix validation is
required before final independent verification.


## 12. Post-audit focused host validation

After the independent-audit M1 opaque-binding fix and the added L1/L4
regressions, the owner reran the dedicated M4P5B host binary under the same
strict-warning ASan/UBSan build.

The compile command succeeded; the first invocation contained only a shell typo
(`/tmp/m4p5b9`). Running the correctly built binary:

```text
/tmp/m4p5b
```

returned silently to the shell prompt.

Result: **PASS**.

This post-fix focused gate covers:

- opaque authenticated packet/plaintext/credential binding;
- replay-before-delivery ordering;
- credential lifetime rejection;
- History incarnation change after replay acceptance;
- selective-set-full after replay acceptance;
- kDeliveryInvariantFailure safe-prefix / consumed-counter behavior;
- fresh-counter recovery;
- reboot replay-boundary host model.

This remains host/state-machine evidence only. It is not physical RF,
power-cut, brownout, gateway/backend or outage-recovery qualification.

Remaining post-fix gates:

- aggregate host regression;
- fresh RAK4630 production build;
- independent focused final verification.


## 13. Post-audit aggregate host regression

After the opaque authenticated-receipt binding fix and added audit regressions,
the owner reran:

```text
bash firmware/tests/run_host_tests.sh
```

Result: **PASS**.

The completed suite preserved PASS across the existing production startup,
radio ownership/listen-window, History/SecurityStore, M4P4/M7P6I vectors,
BLE, storage and tooling guards.

This remains host evidence only.

Remaining post-fix gates:

- fresh RAK4630 production build;
- independent focused final verification.


## 14. Post-audit RAK4630 production build

After the opaque authenticated-receipt binding fix and added audit regressions,
the owner rebuilt the normal production image:

```text
pio run -d firmware -e rak4630

RAM:   29,024 / 248,832 bytes (11.7%)
Flash: 265,356 / 815,104 bytes (32.6%)
SUCCESS — 14.98 s
```

Result: **PASS**.

The normal production footprint remains unchanged:

```text
RAM:   29,024 bytes
Flash: 265,356 bytes
```

This remains consistent with no active production runtime caller for M4P5B.

All owner-side post-audit validation gates are now PASS.

Remaining merge gate:

- independent focused final verification of the exact final branch head.


## 15. Host-test ODR hygiene

Before final independent verification, the M4P5B host-only friend macro was
tightened.

The test capability friend remains absent from production. For the dedicated
host test program, `ORUN_M4P5B_HOST_TEST` is now supplied as a compiler define
to the complete M4P5B test link rather than being defined only inside the test
translation unit. Therefore every translation unit in that test program sees
the same `AuthenticatedBackendDurableReceipt` class definition.

This change affects test compilation only. It does not change the normal
RAK4630 production preprocessor path, runtime behavior or footprint.

A fresh focused host run is required after this test-harness correction.
