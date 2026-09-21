# M7P7D — Transport-neutral application request seam

Status: **PASS WITH FIXES APPLIED — external review findings addressed; post-fix host + RAK4630 revalidation pending. NO BLE APPLICATION GATT OR PROTECTED WRITE PATH.**

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
- read-through to the existing `ConfigStore` owner, with separate
  backend-readiness and committed-record provenance so blank/corrupt/default
  fallback state is not misreported as a recovered durable record;
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
- `config_backend_ready=true` means ConfigStore/backend initialization
  succeeded; it does **not** imply a committed record was recovered;
- `config_has_committed_record=true` means recovery found an actual valid
  committed page;
- blank or corrupt/unrecognized config flash may therefore report
  `config_backend_ready=true`, `config_has_committed_record=false` and the
  safe defaults;
- backend initialization failure reports both facts false and still exposes the
  existing documented safe fallback.

The USB adapter presents this as `source=stored|default`, avoiding the false
equivalence between "store initialized" and "durable setting existed".

## 5. USB surface

Current diagnostic command:

```text
APP CONFIG?
```

Expected shape:

```text
APP RESULT id=<n> code=OK config_backend_ready=yes|no source=stored|default
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

Pre-fix owner validation was performed on `c2232d6` on 2026-09-21:

1. full host suite PASS with ASan/UBSan and warnings-as-errors;
2. all startup composition scenarios PASS;
3. production RAK4630 build PASS;
4. RAM 22,116 / 248,832 bytes = 8.9%;
5. Flash 226,148 / 815,104 bytes = 27.7%;
6. storage-ceiling and exclusive-owner link guards PASS.

The external independent reviewer then independently reported:
- full host suite PASS from a scratchpad copy;
- focused M7P7D GNU++11 + sanitizer PASS;
- pinned ARM GCC 7.2.1 GNU++11 compile PASS;
- final result **PASS WITH FIXES**.

Disposition is recorded in
`docs/audits/M7P7D_EXTERNAL_REVIEW_DISPOSITION.md`.

The external findings caused code/test changes (config provenance semantics,
real-loop startup coverage, BUSY correlation cleanup), so the earlier build
numbers are retained as exact historical evidence for `c2232d6` and are **not**
silently promoted to the current post-fix head. A fresh host suite + production
RAK4630 build is required before merge.

Physical hardware is not required merely to prove this read-only USB seam: no BLE
GATT, RF, persistence format, power policy or hardware-driver behavior changed.
No physical PASS is claimed for M7P7D.

## 8. Remaining gates

After M7P7D, do not jump directly to protected BLE writes.

The next focused work must still define:

- commissioning/authentication ceremony;
- exact application GATT framing/UUIDs and bounded fragment/queue behavior;
- before a second application transport adapter exists, add explicit
  requester/adapter response ownership (or adapter-specific bounded slots) so
  USB cannot consume a BLE result and vice versa;
- preserve callback -> bounded handoff -> loop-owned dispatch; no BLE callback
  may directly execute `ApplicationRequestService`;
- bond-store/Just Works denial-of-service behavior;
- authority-key custody/recovery;
- closure of the fresh-pairing LESC/CC310 coexistence/serialization gate.

DFU/bootloader preservation/authenticity remains M7P8 scope.
