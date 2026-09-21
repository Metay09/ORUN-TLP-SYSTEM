# M7P7D — Transport-neutral application request seam

Status: **IN PROGRESS — SOFTWARE IMPLEMENTATION; NO BLE APPLICATION GATT OR PROTECTED WRITE PATH.**

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

## 7. Validation plan

Required before merge:

1. full host suite with ASan/UBSan and warnings-as-errors;
2. focused M7P7D unit test proving:
   - defaults are read from real ConfigStore;
   - recovered non-default values pass through unchanged;
   - unread response causes BUSY rather than overwrite;
   - unknown request fails closed;
   - ConfigStore begin failure remains distinguishable from durable recovery;
   - no flash program/erase occurs for GET;
3. startup host test proving the actual USB parser routes through the seam and
   preserves role/radio/flash state;
4. RAK4630 production build and RAM/flash delta;
5. static/independent review of the branch.

Physical hardware is not required merely to prove this read-only USB seam unless
software/build review exposes a device-specific uncertainty. No BLE GATT behavior
is changed in this slice.

## 8. Remaining gates

After M7P7D, do not jump directly to protected BLE writes.

The next focused work must still define:

- commissioning/authentication ceremony;
- exact application GATT framing/UUIDs and bounded fragment/queue behavior;
- bond-store/Just Works denial-of-service behavior;
- authority-key custody/recovery;
- closure of the fresh-pairing LESC/CC310 coexistence/serialization gate.

DFU/bootloader preservation/authenticity remains M7P8 scope.
