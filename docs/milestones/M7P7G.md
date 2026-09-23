# M7P7G — Real Bluefruit ORUN application GATT wiring

Status: **HVX TERMINAL-RETURN FIX CANDIDATE — HOST/BUILD REVALIDATION + FOCUSED HARDWARE REGRESSION + INDEPENDENT AUDIT PENDING**

Baseline: `main@9522e391a532f21ef76ee092889d591cb1c2cf78`
(M7P7F / PR #35 merged).
Branch: `feat/m7p7g-ble-app-gatt`.

## 1. Goal

M7P7F froze and host-tested the bounded application transport but deliberately
left it unreferenced by production composition. M7P7G wires that exact contract
to the pinned Adafruit Bluefruit nRF52 1.7.0 runtime so a real phone can perform
the first ORUN application transaction:

```text
phone WRITE request
 -> bounded BLE-task mailbox
 -> loop-owned BleApplicationTransport
 -> shared ApplicationRequestService
 -> ConfigStore read
 -> bounded local response
 -> non-blocking SoftDevice INDICATE
 -> phone HVC confirmation
```

The only operation remains pre-authorization read-only `GET_CONFIG`.

## 2. Hard scope

Implemented:

- the exact M7P7F 128-bit service/request/response UUIDs;
- request characteristic: **WRITE with response only**, max 20 bytes;
- response characteristic: **INDICATE only**, max 20 bytes;
- one fixed four-entry callback->loop ingress FIFO (each frame <=20 bytes);
- callback-visible session generation and stop-and-wait gate;
- HVC confirmation handoff;
- protocol-source `BLE_GATTS_EVT_TIMEOUT` terminal recovery handoff;
- loop-owned application dispatch and session cleanup;
- disconnect cleanup before any admission-policy re-advertise;
- non-blocking indication submission through `sd_ble_gatts_hvx()`.

Explicitly not implemented:

- provisioning or credential transfer;
- application authentication/authorization;
- config writes;
- MESSAGE/commands;
- private location or broad diagnostics;
- Android/backend code;
- service UUID advertising changes;
- additional BLE clients/sessions.

BLE connection and bond status still do not grant ORUN application authority.

## 3. Bluefruit UUID byte order

M7P7F stores UUID constants in canonical RFC4122 printed-string byte order.
Pinned Bluefruit 1.7.0's `BLEUuid(uint8_t[16])` expects the 128-bit byte array
in little-endian SoftDevice order (its `toString()` prints the array in
reverse). M7P7G therefore reverses each frozen constant once into a persistent
16-byte array before calling `BLEService::begin()` /
`BLECharacteristic::begin()`.

The frozen UUID identity is unchanged.

## 4. Callback and ownership boundary

`onBleApplicationWrite()` executes directly in Bluefruit's BLE event task
(`setWriteCallback(..., false)`) and may only copy one bounded frame into
`BleApplicationHandoff` under the existing critical section.

It does **not** call:

- `ApplicationRequestService` or `BleApplicationTransport`;
- ConfigStore/flash;
- Serial;
- monotonic clock;
- SX1262/radio;
- Bluefruit or SoftDevice APIs.

The existing global `onBleEvent()` additionally copies an HVC confirmation
only when its attribute handle equals the ORUN response value handle. It also
copies a protocol-source `BLE_GATTS_EVT_TIMEOUT` as a terminal connection fact.
It never disconnects or performs recovery inside the BLE event task. All
application/session decisions and Bluefruit disconnect requests remain in
`loop()`.

## 5. Stop-and-wait across the real callback boundary

A mailbox without an explicit stop-and-wait gate is insufficient: a request
written while response A is awaiting HVC could otherwise sit in the callback
queue and become valid after A is confirmed. The FIFO capacity is exactly four
frames, matching one maximum-size M7P7F logical request, so valid fragmented
WRITE-with-response bursts do not depend on cooperative-loop scheduling.

M7P7G mirrors M7P7F's stop-and-wait state into
`BleApplicationHandoff::ingress_allowed_`:

- session begin -> ingress open;
- as soon as the loop-owned transport has an outbound response -> ingress
  closes and any queued next request is cleared;
- while response/CCCD/HVC is pending -> callback rejects new request frames;
- when the exact response HVC arrives, callback admission reopens provisionally
  so a peer that immediately writes after confirmation is not spuriously
  dropped; the HVC and frames are only queued, not executed;
- loop consumes HVC before ingress, calls `confirmOutboundFrame()`, and closes/
  clears the FIFO again if no matching confirmed response exists.

This preserves the semantic timing of M7P7F at the physical BLE boundary.

## 6. Indication path: intentionally not BLECharacteristic::indicate()

Pinned Bluefruit 1.7.0 implements
`BLECharacteristic::indicate(conn, data, len)` as a blocking operation that
waits on `BLEConnection::waitForIndicateConfirm()` until HVC. Calling it from
the ORUN cooperative loop would therefore stall GNSS/radio/storage/watchdog
service while the phone delays or never confirms.

M7P7G instead:

1. checks that the response CCCD has indication enabled;
2. peeks the exact <=20-byte M7P7F frame;
3. submits one `BLE_GATTS_HVX_INDICATION` via `sd_ble_gatts_hvx()`;
4. leaves the M7P7F outbound cursor unchanged;
5. receives `BLE_GATTS_EVT_HVC` through the bounded callback handoff;
6. only then calls `confirmOutboundFrame(session_generation)`.

Failed HVX submission never advances the transport cursor and is retried with
the identical frame after a bounded 25 ms minimum retry spacing. No Serial spam
is emitted on retry.

## 7. Session/disconnect ordering

Production still uses one peripheral client. On each connected session the loop
creates a fresh non-zero M7P7F session generation and activates the callback
handoff for the current connection handle.

On disconnect/mismatch the loop:

1. closes/deactivates callback ingress under the critical section;
2. calls generation-gated `BleApplicationTransport::endSession()`;
3. clears local indication state;
4. only afterwards lets `BleAdmissionPolicy` process the disconnect and
   potentially restart advertising.

Therefore an old frame/HVC cannot seed or clear a replacement session.


## 8. GATTS protocol-timeout recovery

Independent review identified a terminal recovery gap in the non-blocking
indication path. Pinned Bluefruit 1.7.0 consumes `BLE_GATTS_EVT_TIMEOUT` by
releasing its own blocking-indication semaphore state but does not disconnect
the connection. ORUN does not use that blocking helper; without an explicit
path, `ble_application_indication_in_flight` and stop-and-wait state could
remain wedged indefinitely.

The candidate fix handles only
`BLE_GATT_TIMEOUT_SRC_PROTOCOL` and deliberately does **not** add an
inactivity/session-duration timeout:

1. BLE event task stamps the exact active session/connection into the fixed
   handoff and immediately closes callback ingress;
2. queued ingress/HVC facts are discarded because ATT progress is terminal;
3. loop consumes the timeout before HVC or request ingress;
4. loop tears down the ORUN application session;
5. loop requests `Bluefruit.disconnect(conn_handle)`;
6. if the disconnect request fails or the edge is delayed, the request is
   retried no faster than once per second and a fresh ORUN application session
   is not created on the timed-out link;
7. once a real disconnect edge is observed, the existing admission policy owns
   the fresh no-client advertising window.

This is protocol-failure recovery, not a connected-client idle watchdog. The
owner-approved policy remains: a healthy connected client is not disconnected
merely because it has been idle for some duration.

### 8.1 Direct HVX terminal-return recovery

A follow-up software audit found a second terminal-progress gap after the
event-based timeout recovery was added. Pinned S140 6.1.1 documents that
`sd_ble_gatts_hvx()` itself may return `NRF_ERROR_TIMEOUT`, meaning no new
GATT procedure can proceed until the connection is re-established. The previous
adapter collapsed every non-success HVX return into the same 25 ms retry path.
If a terminal return were observed without the timeout-event handoff rescuing
the session first, ORUN could keep the outbound response pending and ingress
closed indefinitely.

Code-bearing candidate `af82b2cfda8cd2fcd162666c9d4d2fda1e796e49`
therefore classifies direct HVX submission results:

- `NRF_SUCCESS` with the complete frame consumed -> submitted / await HVC;
- documented transient states (`NRF_ERROR_BUSY`, `NRF_ERROR_INVALID_STATE`,
  `BLE_ERROR_GATTS_SYS_ATTR_MISSING`, `NRF_ERROR_RESOURCES`) -> retain the
  identical frame and retry no faster than the existing 25 ms spacing;
- `NRF_ERROR_TIMEOUT`, impossible partial-success, and other non-retryable
  submission errors -> terminal application-session cleanup followed by the
  same loop-owned, one-second-spaced physical disconnect recovery used by the
  protocol-timeout event path.

No SoftDevice/Bluefruit call is moved into callback context. This change does
not add an idle/session-duration timeout and does not alter M7P7F wire bytes,
GATT properties, TLP v1, RF, storage or authorization semantics.

## 9. Fragment timeout ordering

Each loop tick calls `BleApplicationTransport::poll(now)` before consuming a
queued ingress continuation. This makes the existing 2000 ms partial-frame
timeout effective even if a continuation sat in the callback mailbox until the
first loop tick after expiry.

No background timer is introduced.

## 10. Tests added

`test_m7p7g_ble_application_handoff.cpp` covers:

- zero generation rejected;
- exact connection/session stamping;
- four-frame bounded FIFO / no overwrite / FIFO ordering;
- max-frame bound;
- wrong connection rejection;
- zero-length malformed frame handoff to transport owner;
- stop-and-wait ingress closure clears pre-staged work;
- stale generation controls are no-ops;
- bounded HVC mailbox;
- terminal GATTS-timeout mailbox closes ingress and discards queued ingress/HVC;
- wrong-connection timeout rejection and exact session stamping;
- replacement session clears old ingress/HVC/timeout facts;
- exact disconnect cleanup and reconnect reuse.

The production startup stub additionally exercises the real loop-owned
GET_CONFIG -> non-blocking HVX -> HVC path, a documented transient
`NRF_ERROR_BUSY` submission with 25 ms retry spacing, direct
`NRF_ERROR_TIMEOUT` terminal-return cleanup/disconnect recovery, and the
event-driven GATTS-timeout recovery path. Both terminal paths cover a failed
first physical disconnect request, bounded one-second retry spacing, no fresh
application session on the terminal ATT link, real disconnect observation and
normal advertising restart.

`test_m7p7g_source_contract.py` guards production composition:

- request property remains WRITE, never WRITE WITHOUT RESPONSE;
- response property remains INDICATE;
- write callback is direct bounded handoff (`useAdaCallback=false`);
- non-blocking `sd_ble_gatts_hvx` is used;
- blocking `BLECharacteristic::indicate()` is absent;
- write callback contains no Serial/Bluefruit/SoftDevice/application/config/
  radio/clock work.

## 11. Compatibility/system impact

```text
TLP v1 bytes/sizes:             unchanged
SX1262 RF behavior/airtime:     unchanged
HistoryStore bytes:             unchanged
ConfigStore bytes:              unchanged
SecurityStore bytes:            unchanged
BLE admission timeout/restart:  same owner/policy; app cleanup added before restart
BLE application GATT:           NOW physically wired
provisioning/authorization:     not implemented
config mutation over BLE:       not implemented
MESSAGE/commands:               not implemented
Android/backend:                not implemented
```

The GATT service is not added to the advertising payload, so existing
advertising name/flags and packet size remain unchanged. The phone discovers
the service after connection.

## 12. Validation evidence

Validated code-bearing head:
`6db1a19047b53e49c63e9605661855e9e6113a72`.

Software:

1. full host suite: **PASS**, including all existing regression/startup cases;
2. M7P7G handoff test: **PASS** under the host suite's
   `-Wall -Wextra -Werror` + ASan/UBSan portable flags;
3. M7P7G source ownership/property guard: **PASS**;
4. production `pio run -d firmware -e rak4630`: **SUCCESS**;
5. linked production size: **22,648 B RAM / 233,320 B flash**;
6. delta versus M7P7F reference (22,124 B / 226,292 B):
   **+524 B RAM / +7,028 B flash**;
7. production upload through nRFutil/USB DFU: **SUCCESS** (`Device programmed.`).

Physical RAK4631 + Android nRF Connect:

1. post-upload boot/advertising observed as `ORUN-4B275BA5`; existing bond
   remained valid;
2. after service refresh, exact ORUN application service
   `0088f2c3-cc13-4b5d-b025-f7fcb7c71af2` discovered;
3. exact request characteristic
   `fbf521f6-ce25-4e6a-af6c-c76303803ee3` discovered with **WRITE** only;
4. exact response characteristic
   `c0ba4044-1242-447b-9f79-e97aa0065ad3` discovered with **INDICATE** and
   CCCD `0x2902`;
5. enabling indications succeeded;
6. GET_CONFIG request
   `01-01-03-00-01-00-00-00` produced exact 18-byte response
   `01-81-03-00-01-00-0A-00-00-03-B4-00-00-00-00-00-00-00`;
   decoded result: status OK, backend-ready + committed flags,
   tracking interval 180 s, battery capacity 0 mAh;
7. same-session HVC/stop-and-wait progression verified by a second request
   with correlation `02 00`, which returned correlation `02 00`;
8. disconnect caused the device to reappear advertising, then reconnect
   succeeded and correlation `03 00` completed normally;
9. a START-only partial with correlation `04 00` generated no response;
   after disconnect/reconnect, an END-only continuation for the old
   correlation generated no response, while fresh correlation `05 00`
   completed normally;
10. with indications disabled, correlation `06 00` produced no indication;
    correlation `07 00` was then written while the first response remained
    pending. Re-enabling indications surfaced the pending correlation
    `06 00` response; no correlation `07 00` response was observed during
    this test. This is the physical stop-and-wait/non-reader evidence collected
    for this slice.

The physical evidence above belongs to the previously validated code-bearing
head `6db1a19047b53e49c63e9605661855e9e6113a72`.

Timeout-recovery software revalidation was completed on
`ab2977a63c6f7945935a06ae93d2895dd64938cb`:

1. full `./firmware/tests/run_host_tests.sh`: **PASS**;
2. all production startup scenarios: **PASS**;
3. M7P7G timeout source/ownership guard: **PASS**;
4. host warnings-as-errors + ASan/UBSan coverage in the normal suite: **PASS**;
5. `pio run -d firmware -e rak4630`: **SUCCESS**;
6. linked production size: **22,672 B RAM / 234,264 B flash**
   (**9.1% / 28.7%**);
7. delta versus the prior physically validated M7P7G code head
   (22,648 B / 233,320 B): **+24 B RAM / +944 B flash**;
8. delta versus M7P7F reference (22,124 B / 226,292 B):
   **+548 B RAM / +7,972 B flash**.

No new physical claim is made for the timeout-recovery head. The owner did not
have the device available during that revalidation session, so the focused
hardware regression is explicitly **DEFERRED / NOT YET RUN**, not failed.

The follow-up HVX terminal-return fix at
`af82b2cfda8cd2fcd162666c9d4d2fda1e796e49` is a newer code-bearing
candidate. The PASS results above belong to `ab2977a...` and must not be
carried forward to this candidate until the normal host/startup/source-guard
suite and production RAK4630 build are rerun. No physical claim exists for
`af82b2cf...`.

Remaining before merge:

1. full host warnings-as-errors + ASan/UBSan/startup/source-guard revalidation
   on the latest code-bearing head, followed by the production RAK4630 build
   and size record;
2. focused hardware regression of normal connect -> GET_CONFIG -> HVC ->
   disconnect/re-advertise behavior on that exact revalidated head;
3. direct physical injection of a genuine ATT protocol timeout is desirable if
   a practical test client can withhold HVC, but must not be claimed if the
   available phone client automatically confirms indications;
4. independent audit and any resulting fix/retest cycle.

Physical validation does not imply broader provisioning, authorization,
config-write, messaging, RF or backend behavior; those remain outside M7P7G
scope.
