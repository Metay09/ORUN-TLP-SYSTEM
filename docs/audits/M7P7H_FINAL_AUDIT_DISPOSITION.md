# M7P7H Independent Final Audit Disposition

Status: **PASS WITH FIXES — ACCEPTED FIXES APPLIED; OWNER REVALIDATION PENDING.**

Audit target:

- PR: #61
- branch: `feat/m7p7h-read-only-device-status`
- audited head: `08436a684b86f6b3e48921be66de2e4a6c6e438c`
- baseline: `main@8f5f8b75e2b5c27d10e4dfec3130267afc300314`
- immediate runtime baseline: `main@034d0afdbd7b4e26fd2cd44310486f20a61307d7`
- audit date: 2026-10-02

The reviewer independently ran `firmware/tests/run_host_tests.sh` on the audited candidate and reported EXIT=0. The reviewer did not independently run the RAK4630 production build or physical hardware qualification; those remain owner evidence gates.

## Verdict

**PASS WITH FIXES**

- BLOCKER: 0
- HIGH: 0
- MEDIUM: 3
- LOW: 4

No TLP v1, RF/airtime, flash-format, BLE callback-ownership or GET_CONFIG compatibility regression was found. Findings were concentrated in status truthfulness, wire/golden coverage, pre-authorization classification and stale documentation.

## Accepted findings and fixes

### M1 — resolved relay intent vs actually applied forwarding

Accepted and fixed. DEVICE bytes 10..11 continue to describe resolved relay service state/reason, while DEVICE byte 3 bit2 now reports the actual `RadioManager::relayForwardingEnabled()` state. USB DEVICE output exposes `relay_applied=yes|no`. Startup coverage forces resolved RELAY intent while actual forwarding remains false and verifies the distinction.

### M2 — real composition and complete new wire layouts under-tested

Accepted and fixed.

- DEVICE, TRACKING/GNSS, GEOFENCE and STORAGE now have full logical byte-for-byte BLE golden vectors.
- Request types `0x02..0x05` are tested as zero-payload contracts; well-framed nonzero payloads fail closed with no application response slot.
- `UNAVAILABLE -> ERROR 0x04` is wire-tested.
- `ACCESS_DENIED 0x03`, `UNAVAILABLE 0x04` and `UNSUPPORTED 0x01` application-to-BLE mappings are centralized and compile-time guarded.
- Production startup coverage exercises the real owner-to-snapshot composition path and checks tracking requested/applied/effective values, firmware version, geofence owner state/counts, HistoryStore counters/readiness and Config/Geofence/Security readiness/maintenance facts.

### M3 — new open-BLE fields lacked explicit pre-authorization classification

Accepted and fixed in documentation. M7P7H now records a field-level development/local pre-authorization classification and explicitly calls out that geofence state, reset/uptime and SecurityStore state can reveal operational information. Coordinates, geometry, raw state-token bytes, credentials/keys, protected writes, MESSAGE and COMMAND/RESULT remain excluded. Before field/customer deployment or private-person/location use, sensitive fields/families require reviewed authenticated/authorized access policy.

### L1 — 32-bit uptime wraps

Accepted and fixed semantically/documented. The field is explicitly `uptime_ms_mod32`: low 32 bits of monotonic milliseconds since boot, wrapping about every 49.7 days. It is diagnostic continuity only, not wall-clock or lifetime uptime. No speculative 64-bit uptime owner was introduced.

### L2 — snapshot freshness depended on adapter discipline

Accepted and fixed. `ApplicationStatusSnapshot` now has a non-wire `populated` validity bit. The composition builder publishes it last; `ApplicationRequestService` returns `UNAVAILABLE` if the snapshot is absent or has never been populated. A future LoRa/gateway adapter therefore cannot accidentally return the zero-initialized boot snapshot as authoritative.

### L3 — BLE link security fact is not yet populated

Accepted as an explicit current limitation. M7P7H currently submits BLE requests as `kBleOpen`. `kBleEncrypted` is an insertion seam only; actual Bluefruit link-security state is not yet sampled. That is conservative for the current all-open read-only development allowlist, but real link-security state must be wired before any rule distinguishes open from encrypted BLE or before protected/sensitive field operation.

### L4 — stale GET_CONFIG-only comments/docs

Accepted and fixed in `ble_application_transport.h`, M7P7F, current architecture rules, gap analysis and architecture README. M7P7F remains the historical frozen GET_CONFIG base; M7P7H is documented as an additive read-only extension.

## Compatibility disposition

```text
TLP v1 packet bytes/sizes:       unchanged
SX1262 RF behavior/airtime:      unchanged
persistent flash formats:        unchanged
GNSS power policy:               unchanged
BLE UUIDs/properties/framing:    unchanged
GET_CONFIG BLE 0x01/0x81:        unchanged
GET_CONFIG USB output:           unchanged
BLE callback ownership:          unchanged
M7P7H status wire:               semantics corrected before physical freeze
LoRa application adapter:        not implemented
protected writes/security:       not implemented
```

M1 uses a previously unused DEVICE flag bit and keeps the DEVICE logical payload at 36 bytes. L1 clarifies the existing four-byte uptime field. The `populated` validity bit is internal RAM state only and is never serialized.

## Evidence boundary after fixes

The earlier owner host/build results remain historical evidence for the pre-fix candidate. Because accepted audit findings changed firmware and tests, they are not promoted to the corrected head.

Post-fix owner revalidation is complete on code-bearing head
`20fcc417a242558e24c2ab73ee557157737f18cc`:

- full `firmware/tests/run_host_tests.sh`: **PASS**;
- warnings-as-errors / ASan / UBSan coverage: **PASS**;
- all production startup scenarios: **PASS**;
- production `pio run -e rak4630`: **SUCCESS**;
- corrected size: **28,936 B RAM / 264,624 B flash**
  (**11.6% / 32.5%**);
- delta versus PR #58 immediate production baseline:
  **+192 B RAM / +4,424 B flash**;
- audit-fix-only delta versus the earlier M7P7H build:
  **+8 B RAM / +88 B flash**.

Physical qualification on the corrected code-bearing firmware is now
**PARTIAL PASS**:

- upload: PASS;
- boot/runtime sanity with GNSS fix, POSITION TX and HistoryStore append: PASS;
- BLE connect/GATT discovery and indications enable: PASS;
- GET_CONFIG regression: PASS;
- DEVICE: PASS, 36 bytes / 3 indication fragments;
- TRACKING/GNSS: PASS, 36 bytes / 3 indication fragments;
- GEOFENCE: PASS, 9 bytes / 1 indication fragment;
- STORAGE: PASS, 40 bytes / 4 indication fragments.

The connection later dropped after the operator moved out of BLE range. That
does not count as a failure, but it also does not satisfy the controlled
disconnect/reconnect acceptance check.

Remaining gate:

1. when hardware is available again, connect to the same firmware and perform
   **disconnect -> reconnect -> GET_CONFIG indication**;
2. record that exact result;
3. only then claim full M7P7H physical PASS and make PR #61 merge-ready.

No full M7P7H physical PASS is claimed here.
