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
5. Only the FIRST failed-contact episode gets ONE extra retry at half the
   current report interval: for 1h reports, +30min; for 2h reports, +1h.
   Afterwards, remain radio-quiet until the report owner announces another
   durably committed PERIODIC via `onReportCommitted()`. That contact attempt
   reuses the SAME cached object, with no new AEAD/counter and NO extra
   half-period retry. There is no 30s→15min retry escalation.
   Radio-busy defers without flash scanning each loop; empty-store scans
   remain throttled to 30s.
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

Tests include cache-before-send, single half-report retry, report-boundary
contact probes, quiet offline periods, ACK-and-release-gated backlog pacing,
identical retries, reboot reuse without
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

## 2026-10-15 source review and independent focused verification

- Corrected exponential retry delay to reach the documented 15-minute cap
  (30/60/120/240/480/900 seconds, then capped at 900 seconds). Added a
  wrap-safe boundary regression for every scheduled attempt.
- On cached-object recovery, the send owner now compares the first read to a
  second persisted read before accepting the cache; a difference fails closed
  without sealing or transmission. A targeted mutation-between-reads
  regression proves `tx=0` and `State::kFault`.
- Independently compiled **byte-for-byte matching GitHub source blobs** in an
  isolated container (same SHA as GitHub contents API) using GCC C++11 with
  ASan/UBSan/`-Werror`, GCC `-Wconversion -Wsign-conversion`, and Clang C++11
  with ASan/UBSan/`-Werror`/`-Wconversion -Wsign-conversion`. Every focused
  execution returned `SF5J2B1 dormant product send coordinator: PASS`.
- Full project host suite/PlatformIO are **unverified in this environment**:
  no repository checkout or `pio` binary is available. GitHub commit CI
  statuses also have not been reported for the current branch.
- Important remaining integration gate: real production adapter needs a
  independently reviewed, durable credential-lifetime binding before cached
  bytes may be sent. `currentCredentialOwns` is intentionally abstract.
  A non-activated sender does not qualify live RF, custody, power behavior,
  or Gateway/Edge durability.
- SF5J2B1 stays **Draft**. Do not activate production RF, merge to main,
  erase/prepare flash, provision credentials, or physically upload firmware
  solely on the basis of the focused portable tests.

## 2026-10-15 follow-up: verification and battery guard

- Independently mirrored the three focused sender source files using GitHub
  contents and checked their **exact Git blob SHA** against the remote sources
  before compiling locally.
- GCC and Clang C++11 with `-Wall -Wextra -Werror`,
  `-Wconversion -Wsign-conversion`, ASan and UBSan: focused sender
  regression suite **PASS** after the unavailability throttle patch.
- One source review found `ProductSendSeal::kUnavailable` retried every loop
  and re-read ObservationStore, potentially scanning a 128 KiB region
  repeatedly during SecurityStore reservation contention. The sender now
  schedules a **60-second scan/retry interval**, with a host regression
  asserting no intermediate flash reads, seals, writes or TX.
- A GitHub Actions workflow was created to request the full host test suite
  and a compile-only `rak4630` build. Two separate workflow attempts
  reported `failure` **before executing any steps** (both jobs had empty
  step arrays and no executable job logs). Therefore **neither full suite
  nor target build passed or ran** from those Actions attempts. Treat the
  runner/startup failure as an unresolved infrastructure gate, not code-test
  evidence. Keep the PR Draft pending successful runs.
- No physical upload, radio activation, provisioning, product-partition
  PREPARE/erase, authenticated custody ACK, or production record release.

### Credential binding direction for a later reviewed slice

Existing SF5C exact-object bytes lack `credential_id`; the same device
identity and key epoch can belong to a *different* credential after
reprovision. Neither a CRC nor an unchanged object proves key ownership.
A possible **new, explicitly audited** option is an internal verification-only
cryptographic check: with the current credential, recompute the canonical
PERIODIC protected object deterministically from the *retained logical
plaintext* using the cached frame's nonce/counter, and compare all ciphertext
and tag bytes in constant time, without consuming a new TX counter or
publishing a newly sealed object. This requires a separate ProductSecureCrypto
verification API/contract, crypto-engine KATs, and an audit of nonce, plaintext
binding and lifetime transitions; the current `protectNextPeriodic()` must
**not** be reused because it reserves a fresh counter. The option is a
proposal, not authority to change the existing seal-only security owner.

Until such a proof or another reviewed durable binding exists,
`ProductSendPort::currentCredentialOwns()` must return false and no
production PRODUCT_SECURE TX can be activated by this PR.

## 2026-10-15 owner-requested link-aware retry change

The original exponential 30s→15min timer is RETIRED in this Draft PR.
The new **report-cadence-aligned stop-and-probe** policy is:

1. Select the oldest retained PERIODIC. If it has cached exact bytes,
   recover them first. Otherwise seal once and durably cache before the first
   on-air opportunity.
2. On initial TX attempt without authenticated custody, permit one extra
   attempt exactly half the configured report interval later.
3. No further standalone retries. When the *existing* report owner finishes
   committing the next report period, the dormant sender may make ONE contact
   probe using its PREVIOUSLY cached object, then stays quiet again.
   Do not use an internal timer to pretend that a new report was committed.
4. On a separately authenticated, exact-object-matched Gateway custody ACK
   **and durable local record release**, a future trusted adapter may call
   `onCustodyReleaseCommitted(identity, now_ms)`. The sender then waits at
   least 15 seconds before it selects/sends the next oldest retained record.
   That next record needs its **own** verified ACK; an ACK for the newest
   record is not a blanket ACK for the earlier five.
5. A tracker can retain 5, 6 or many logical observation records while its
   control journal admits at most four actively encrypted exact objects.
   To preserve the bound while disconnected, this single-in-flight policy
   resends the existing oldest exact frame as the periodic reachability probe.
   **It does not guarantee the packet sent during the sixth reporting period
   is the sixth report.** Once custody for the old frame is proven, previously
   retained reports can drain oldest-first with pacing and per-object ACKs.
6. `reportPeriodMs()` is injected by the future real config/report owner.
   Invalid cadence never authorizes rapid retries. This slice is dormant and
   cannot authenticate ACK or mutate release state itself.

At host scope, test both 1-hour and 2-hour intervals, no transmission while
offline between report events, old exact-frame reuse, selective release
notification, 15-second drain pacing, and missing cadence behavior. Actual
radio RX-window scheduling, awake duration, power draw, duty cycle, Gateway
receipt handling, and physically measured convergence remain **unproven**.

No production adapter, Gateway ACK authority, physical flash work, firmware
upload, or TLP v1 retirement is added. Full host/PlatformIO verification
is still required; GitHub Actions previously failed before running any steps.
