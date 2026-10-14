# SF5J2B1 — Dormant PRODUCT_SECURE send coordinator (candidate)

Baseline: `main@20d71f6dbfa64c281ea181492d6ad5aa1ba20350`
Branch: `feat/sf5j2b1-dormant-product-send-owner`

## Scope

Implements a minimal non-blocking, **production-dormant** `ProductSendOwner`
state machine behind an injected `ProductSendPort` boundary. It does **not**
bind a production port in `main.cpp`, call live RadioManager, provision keys,
write real ProductRecordStore records, or enable secure RF. Existing
POSITION/HISTORY_SECURE behavior is untouched.

Only PERIODIC 64-byte logical records and 100-byte exact PRODUCT_SECURE frames
are candidates in this initial slice. EVENT, RESULT, selective out-of-order
custody release, and priority arbitration are not implemented.

## Cache-first invariants

1. If an exact object already exists for the selected retained record, use
   it without invoking encryption or reserving a new security counter.
2. Otherwise, an injected future adapter must seal *only* via the existing
   SecurityStore-owned `ProductSecureCrypto`, then submit those exact bytes
   to the single `ProductRecordStore` owner.
3. A started cache write is never TX eligibility. Await its durable outcome,
   then read back and byte-compare the entire object. A mismatch, invalid
   record or read failure fails closed.
4. Every scheduled RF attempt re-checks retained backing record, unchanged
   persisted exact bytes and active credential-lifetime authority. TX results
   do not constitute custody and never release the record.
5. Local attempt backoff starts at 30 s and doubles to a cap of 15 min.
   Radio-busy defers without scanning flash on every loop. Empty-store
   rescans are throttled to 30 s, avoiding repeated 128 KiB reads.
6. Faulted cache/crypto operations do not auto-reseal on a timer.
   After an uncertain flash mutation, a future separately reviewed
   recovery policy must decide whether new protection is safe.

### Mandatory security gate before a production adapter

The SF5C exact-object journal persists header, ciphertext and tag, but **does
not include a full `credential_id`**. Matching `key_epoch` alone does not
prove that a reboot-recovered exact object belongs to the currently active
credential lifetime after security reset/reprovision.

`ProductSendPort::currentCredentialOwns()` is deliberately mandatory and
must be backed by reviewed durable credential-lifetime authority; otherwise
it must return false. Do not synthesize proof from `key_epoch`, generic
DeviceIdentity or an unverified wire header. No default production adapter
is provided by SF5J2B1, so this gate cannot be bypassed by deployment.

`ProductSendPort` methods are expected to translate the existing
`ObservationStore::Record`, `ProductRecordStore::findCachedExact`,
`ProductSecureCrypto::protectNextPeriodic` and
`RadioManager::sendProductSecurePacket` interfaces **later**, after the
security-binding and runtime gates are designed. These are not wired today.

## Verification

Standalone portable host compilation and execution performed in an isolated
workspace:

```sh
g++ -std=c++11 -O1 -g -Wall -Wextra -Werror \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  -Ifirmware/include firmware/tests/sf5/test_sf5j2b_sender.cpp \
  firmware/src/product_send_owner.cpp -o sf5j2b_sender
./sf5j2b_sender
```

Result: `SF5J2B1 dormant product send coordinator: PASS`.

Tests include cache-before-send, identical retries, reboot reuse without
re-sealing, unprovisioned/credential-mismatch refusal, cache-write failure,
owner readback failure, radio BUSY/send rejection, changed exact bytes,
released backing, idle-store throttling and millisecond counter rollover.

Full repository host suite and RAK4630 PlatformIO target build are **not yet
executed for this branch** (do not misreport). The new host test is added to
`firmware/tests/run_host_tests.sh` and must be run on developer checkout.
No physical upload/erase/PREPARE, runtime crypto activation, Gateway ACK or
product cutover is authorized by this milestone.

## Before SF5J2B1 can be ready to merge

- Independent source review of the sender state machine and power/retry policy.
- Full host suite, ASan/UBSan, `-Werror` and RAK4630 build on this exact SHA.
- Confirm target compilation of dormant CPP within one firmware image.
- Evaluate stack/RAM footprint and `ProductSendPort` lifetime design.

## Future SF5J2B2 / RF qualification

- Reviewed durable credential-lifetime binding for cached objects.
- Real production adapter and EVENT family integration.
- Counter reservation and AEAD end-to-end with real `SecurityStore`.
- Field duty-cycle/power/backoff policy and explicit service enablement.
- Gateway durable custody and authenticated ACK verification before record
  release; no TX_DONE shortcut.
- Two-node physical RF and power-cut/SoftDevice validation before product
  cutover.
