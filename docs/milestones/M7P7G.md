# M7P7G — Real Bluefruit ORUN application GATT wiring

Status: **IMPLEMENTED CANDIDATE — HOST/BUILD/PHYSICAL VALIDATION PENDING**

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
- one fixed 20-byte callback->loop ingress mailbox;
- callback-visible session generation and stop-and-wait gate;
- HVC confirmation handoff;
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
only when its attribute handle equals the ORUN response value handle. All
application/session decisions remain in `loop()`.

## 5. Stop-and-wait across the real callback boundary

A one-frame mailbox alone is insufficient: a request written while response A
is awaiting HVC could otherwise sit in the callback mailbox and become valid
after A is confirmed.

M7P7G mirrors M7P7F's stop-and-wait state into
`BleApplicationHandoff::ingress_allowed_`:

- session begin -> ingress open;
- as soon as the loop-owned transport has an outbound response -> ingress
  closes and any queued next request is cleared;
- while response/CCCD/HVC is pending -> callback rejects new request frames;
- only after final HVC drives `confirmOutboundFrame()` and the outbound
  response is gone -> ingress reopens.

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

## 8. Fragment timeout ordering

Each loop tick calls `BleApplicationTransport::poll(now)` before consuming a
queued ingress continuation. This makes the existing 2000 ms partial-frame
timeout effective even if a continuation sat in the callback mailbox until the
first loop tick after expiry.

No background timer is introduced.

## 9. Tests added

`test_m7p7g_ble_application_handoff.cpp` covers:

- zero generation rejected;
- exact connection/session stamping;
- one-frame bounded mailbox / no overwrite;
- max-frame bound;
- wrong connection rejection;
- zero-length malformed frame handoff to transport owner;
- stop-and-wait ingress closure clears pre-staged work;
- stale generation controls are no-ops;
- bounded HVC mailbox;
- replacement session clears old ingress/HVC;
- exact disconnect cleanup and reconnect reuse.

`test_m7p7g_source_contract.py` guards production composition:

- request property remains WRITE, never WRITE WITHOUT RESPONSE;
- response property remains INDICATE;
- write callback is direct bounded handoff (`useAdaCallback=false`);
- non-blocking `sd_ble_gatts_hvx` is used;
- blocking `BLECharacteristic::indicate()` is absent;
- write callback contains no Serial/Bluefruit/SoftDevice/application/config/
  radio/clock work.

## 10. Compatibility/system impact

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

## 11. Required validation before merge

Software:

1. focused M7P7G handoff test with `-Wall -Wextra -Werror` + ASan/UBSan;
2. M7P7G source ownership/property guard;
3. full host suite;
4. production `pio run -d firmware -e rak4630`;
5. record RAM/flash delta versus M7P7F (22,124 B / 226,292 B);
6. independent audit and any fix/retest cycle.

Physical RAK4631 + phone:

1. flash production candidate and verify normal boot/admission behavior;
2. nRF Connect sees existing `ORUN-XXXXXXXX` name;
3. after connect, discover exact ORUN service + request/response UUIDs;
4. enable indications on response characteristic;
5. WRITE WITH RESPONSE an 8-byte GET_CONFIG frame;
6. receive and decode exact 18-byte GET_CONFIG indication;
7. disconnect/reconnect and repeat with no stale response;
8. partial-fragment then disconnect/reconnect leaves no stale partial;
9. non-reader/unconfirmed-response behavior does not execute a second request;
10. confirm existing post-disconnect advertising restart/admission still works.

Do not claim physical PASS until those observations are actually collected.
