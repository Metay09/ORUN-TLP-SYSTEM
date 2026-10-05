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

A future runtime caller must first obtain all of these from the same successful
`HistorySecureCrypto::openBackendDurableReceipt()` call:

- decoded `HistorySecurePacket`;
- decoded `BackendDurableReceiptPlaintext`;
- exact authenticated `credential_id` snapshot.

M4P5B then submits exactly:

```text
authenticated_credential_id
packet.key_epoch
packet.security_counter
```

to `SecurityStore::submitAuthenticatedA2dCounter()`.

It must never re-read `currentCredentialId()` for an already-authenticated
receipt.

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

It performs structural cross-checks only to catch future caller misuse:

- packet must be valid frozen `HISTORY_SECURE`;
- context must be `BACKEND_A2D`;
- family must be `BACKEND_DURABLE receipt`;
- packet ciphertext length must match the supplied receipt count;
- supplied receipt must satisfy the frozen explicit-ID plaintext rules.

These checks are defense-in-depth after AEAD success, not a second authority.

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
