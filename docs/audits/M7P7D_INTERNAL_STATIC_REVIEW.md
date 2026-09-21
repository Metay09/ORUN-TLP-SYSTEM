# M7P7D Internal Static Review

Date: 2026-09-21
Baseline: `main@6774e7425a3776987ddaaff749c01d5cb20474c1`
Branch: `feat/m7p7d-app-request-seam`

Status: **PASS — no code-blocking finding. External independent review still pending.**

This review is an internal repository/static review. It is not Astra evidence,
not a hardware test and not a substitute for the owner-requested independent
final audit step.

## Scope reviewed

- `firmware/include/application_request.h`
- `firmware/src/application_request.cpp`
- production composition/USB adapter in `firmware/src/main.cpp`
- focused M7P7D host tests
- production startup test integration
- M7P7D milestone and current architecture/index changes
- interaction with the existing `ConfigStore` ownership contract

## Findings

### 1. Application ownership

PASS.

The new service reads the existing `ConfigStore`; it does not duplicate config
state or create a BLE/USB-owned configuration model.

The production USB adapter is executed by the existing main-loop serial parser.
No new callback/task writes application state.

### 2. Bounded memory / backpressure

PASS.

One response slot is fixed-memory backpressure. An unread result makes the next
submission return `BUSY`; no overwrite, heap queue or unbounded accumulation
exists.

The BUSY attempt does not consume the local USB correlation ID. That is acceptable
for this diagnostic surface: the ID identifies the next request that can actually
be accepted, not an application MESSAGE/security identity.

### 3. Storage / power-cut / flash ownership

PASS.

`GET_CONFIG` calls only `ConfigStore::ready()` and `ConfigStore::config()`.
It does not call `requestSave()`, `poll()`, a flash backend or
`FlashMutationGate`.

Therefore this slice does not alter flash wear, power-cut recovery, persistent
record layout or the existing shared SoftDevice flash ownership path.

### 4. RF / protocol / mixed fleet

PASS.

No TLP serializer/deserializer, RF packet, relay, PHY, sequence or airtime path is
changed. The USB diagnostic is local and emits no SX1262 traffic.

### 5. Security boundary

PASS for current scope.

The exposed fields are the already-existing tracking interval and battery-capacity
configuration. No credential/root-key/replay/private-location/message material is
returned.

The USB adapter is deliberately read-only. This review does **not** authorize
future protected config writes over USB/BLE without the commissioning/
authorization design.

### 6. ConfigStore readiness semantics

PASS.

When `ConfigStore::begin()` fails, its documented safe defaults remain readable,
but M7P7D separately returns `config_store_ready=false`. A client therefore has
enough information not to misrepresent fallback defaults as durable recovered
state.

### 7. Future async operations

NON-BLOCKING DESIGN NOTE.

`submit()` currently resolves synchronously because the only operation is a
read-only snapshot. Do not extend this exact synchronous assumption to future
flash writes, provisioning, MESSAGE or command execution. Those operations may
need an explicit accepted/pending/completed state while preserving the same
bounded owner-loop discipline.

No code change is required for M7P7D.

### 8. Physical validation

No new device-specific behavior requiring physical validation was found.

The slice adds a local read-only Serial request path and one small in-memory
service. BLE GATT, SoftDevice callbacks, flash writes, RF, GNSS, sensor drivers
and power policy are unchanged. Host startup coverage exercises the real
production composition path with the service linked.

## Validation observed

Owner-reported 2026-09-21 validation:

- full host suite: PASS;
- ASan/UBSan + warnings-as-errors host tests: PASS;
- production RAK4630 build: PASS;
- RAM: 22,116 / 248,832 bytes;
- Flash: 226,148 / 815,104 bytes;
- delta from preceding production binary: +32 RAM, +648 Flash.

Only existing third-party SX126x-Arduino build warnings were visible in the
reported build output.

## Result

No blocker found.

Remaining merge gate: external independent review of PR #32. No physical test is
requested for this slice unless that review finds a hardware-specific concern.
