# M7P7D — Transport-neutral application request seam

Status: **SOFTWARE PASS — host/sanitizer + RAK4630 production build PASS; static review PASS; external independent review pending. NO BLE APPLICATION GATT OR PROTECTED WRITE PATH.**

Baseline: `main@6774e7425a3776987ddaaff749c01d5cb20474c1` (PR #31 / M7P7C design gate merged).
Branch: `feat/m7p7d-app-request-seam`.

## 1. Why this slice exists

M7P7C established that BLE must remain a transport adapter rather than becoming
a second configuration/business-logic owner. Before exact GATT UUIDs or byte
framing are frozen, ORUN needs one small typed application seam that can be
host-tested and exercised through an existing local transport.

M7P7D therefore adds the smallest real seam needed to prove that direction.

It does **not** implement provisioning, authorization, secure RF, MESSAGE,
commands, protected config mutation, Entity Registry, Android or backend.

## 2. Scope

M7P7D adds:

- a fixed-memory, loop-owned `ApplicationRequestService`;
- a typed `GET_CONFIG` request and typed response;
- one response slot with explicit BUSY backpressure;
- read-through to the existing `ConfigStore` owner, including a separate
  `config_store_ready` fact so safe fallback defaults are not misreported as
  recovered durable configuration;
- a read-only USB diagnostic adapter: `APP CONFIG?`;
- host coverage for the production request service and startup/USB composition.

The typed C++ request is **not a BLE/USB/LoRa wire format**. Exact GATT UUIDs,
MTU/framing, fragmentation and request bytes remain later work.

## 3. Ownership and security boundary

```text
USB diagnostic adapter
        |
        v
ApplicationRequestService
        |
        v
existing ConfigStore
```

The adapter does not own configuration state.

The application service does not write flash, call Bluefruit, touch the SX1262,
run CryptoCell, infer authorization from a bond, or reinterpret Role.

M7P7D deliberately exposes only a non-secret read operation. No production
transport receives a durable config write path before the commissioning/
authorization design exists.

A future BLE callback must not call `ApplicationRequestService::submit()`
directly from BLE task context. It may only perform the bounded handoff defined
by the later GATT adapter; application dispatch stays in the ORUN owner loop/task.

## 4. Bounded request/result semantics

The service owns exactly one response slot.

A request submitted while the previous response is unread returns `BUSY`.
The prior result cannot be overwritten. No heap allocation, unbounded queue or
generic event bus is introduced.

Unknown typed request kinds return `UNSUPPORTED` with no storage/radio/security
side effect.

For `GET_CONFIG`:

- response data comes directly from `ConfigStore::config()`;
- `config_store_ready=true` means the durable owner initialized successfully;
- if ConfigStore initialization failed, its existing documented safe fallback is
  still returned but `config_store_ready=false`.

This distinction prevents UI/diagnostics from presenting a fallback value as a
durably recovered setting.

## 5. USB surface

Current diagnostic command:

```text
APP CONFIG?
```

Expected shape:

```text
APP RESULT id=<n> code=OK config_ready=yes|no
tracking_interval_seconds=<seconds> battery_capacity_mah=<mAh>
```

The production output is one line; the wrap above is documentation only.

USB request IDs are local diagnostic correlation IDs. They are not MESSAGE IDs,
security counters, protocol sequence numbers or future GATT wire IDs.

## 6. Compatibility / system impact

```text
TLP v1 bytes/sizes:             unchanged
RF PHY/airtime/forwarding:     unchanged
HistoryStore bytes:            unchanged
ConfigStore bytes:             unchanged
SecurityStore bytes:           unchanged
BLE runtime/admission:         unchanged
BLE application GATT:          not implemented
provisioning/authorization:    not implemented
durable config write over APP: not implemented
MESSAGE/commands:              not implemented
Android/backend:               not implemented
```

RAM impact is one small service object, one response record and one USB request
counter. No new durable allocation is introduced.

## 7. Validation evidence

Owner Debian validation on 2026-09-21:

1. **Full host suite: PASS**
   - ASan/UBSan and warnings-as-errors host runner completed;
   - all existing compatibility, RF, storage, BLE/persistence and startup
     regression checks remained PASS;
   - focused M7P7D test was built/executed by the host runner;
   - startup composition scenarios (mutex/gate/queue/lora/success/advfail/
     blefail/noevent) all remained PASS with the M7P7D service linked.
2. **RAK4630 production build: PASS**
   - RAM: 22,116 / 248,832 bytes = 8.9%;
   - Flash: 226,148 / 815,104 bytes = 27.7%;
   - production storage-ceiling and exclusive-owner link checks passed.
3. **Size delta versus the immediately preceding production image**
   (`main@6774e742...` is docs-only relative to the previously measured
   production binary):
   - RAM: +32 bytes (22,084 -> 22,116);
   - Flash: +648 bytes (225,500 -> 226,148).
4. **Static review: PASS with no runtime blocker.**
   - `GET_CONFIG` has no flash/radio/security side effect;
   - unread response cannot be overwritten;
   - current USB adapter executes from the existing loop-owned parser;
   - no BLE callback/application task ownership is introduced;
   - no wire/storage format changes are introduced.

The PlatformIO build still emits pre-existing warnings from the pinned
SX126x-Arduino dependency (RAK4630 preprocessor warning and signed/unsigned
warnings in its RAK11300 SimpleTimer source). No new ORUN-source warning was
identified in this validation.

External independent review remains pending before merge.

Physical hardware is not required merely to prove this read-only USB seam: no BLE
GATT, RF, persistence format, power policy or hardware-driver behavior changed.
No physical PASS is claimed for M7P7D.

## 8. Remaining gates

After M7P7D, do not jump directly to protected BLE writes.

The next focused work must still define:

- commissioning/authentication ceremony;
- exact application GATT framing/UUIDs and bounded fragment/queue behavior;
- bond-store/Just Works denial-of-service behavior;
- authority-key custody/recovery;
- closure of the fresh-pairing LESC/CC310 coexistence/serialization gate.

DFU/bootloader preservation/authenticity remains M7P8 scope.
