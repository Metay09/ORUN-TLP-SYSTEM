# M6D3C — Durable geofence snapshot -> M6D2 runtime provider

Status: **IMPLEMENTATION COMPLETE; COMPLETE HOST / WARNINGS / ASAN / UBSAN PASS; RAK4630 BUILD + AUDIT PENDING**.

Baseline: `main@89d41e0f8f46978ff487cecbd99ce9d09ea5e2fa`.
Branch: `feat/m6d3c-geofence-runtime-provider`.

## 1. Purpose

M6D3B closed durable GeofenceStore persistence, A/B recovery, reboot
persistence and the focused commit-last electrical power-cut boundary, while
deliberately leaving production M6D2 unconfigured.

M6D3C connects that already-recovered durable semantic snapshot to the existing
M6D2 runtime owner at boot.

The user-visible consequence of this slice is narrow but important:

- a durable CONFIGURED geofence can survive reboot and become the local runtime
  geofence automatically;
- durable CLEAR leaves runtime geofence evaluation disabled;
- no hard-coded production polygon is introduced.

This slice does **not** add a BLE/LoRa writer, secure mutation protocol, EVENT
transport, backend alarm or UI.

## 2. Ownership boundary

Ownership remains:

```text
GeofenceStore
  owns durable snapshot/token/recovery
        |
        | recovered semantic snapshot only
        v
geofence_runtime_provider
  reconstructs bounded polygon views
        |
        v
GeofenceConfirmationCoordinator / GeofenceRuntime
  owns runtime geometry + M6D confirmation state
```

The provider owns no flash, GNSS, PositionFlow, radio, BLE or transport.

Role, Location Source, GNSS Power, Capability, Transport, Identity, Profile and
User Identity remain independent.

## 3. Boot ordering

Production now instantiates exactly one:

- `NrfGeofenceIncarnationSource`;
- `GeofenceStore(storage_flash_gate.geofencePort(), ...)`.

`geofence_store.begin()` runs before `Bluefruit.begin()`, preserving the
M6D3B physically-qualified SoftDevice-disabled boot recovery/baseline path.

A blank geofence partition is still not interpreted as CLEAR merely because it
is erased. GeofenceStore first establishes its committed CLEAR baseline exactly
as frozen by M6D3B.

## 4. Semantic state versus mutation token

M6D3C deliberately does not conflate:

`ResourceState != TokenState`.

M6D3B can preserve a defensible read-only CLEAR/CONFIGURED semantic snapshot
while the mutation/CAS token is UNCERTAIN.

Therefore production runtime activation is based on the recovered semantic
snapshot:

- resource UNAVAILABLE / no snapshot -> runtime remains unconfigured;
- CLEAR -> coordinator.clear();
- CONFIGURED -> reconstruct bounded area views and coordinator.configure().

Token state remains observable and controls future mutation authority; it is not
used to discard a semantic fallback that GeofenceStore explicitly preserved.

## 5. Runtime replacement semantics

Applying a CONFIGURED snapshot uses the existing coordinator configure path.

Therefore a successful geometry replacement:

- resets evidence tied to prior geometry;
- resets operational classification;
- leaves cadence at base B;
- emits no synthetic OUTSIDE transition event solely because configuration
  changed;
- requires fresh accepted Location evidence for the new snapshot.

Applying CLEAR:

- disables runtime geofence evaluation;
- clears prior operational authority;
- cancels coordinator confirmation state;
- restores coordinator cadence mode to base.

M6D3C activates only boot-time application. There is still no production
runtime writer, so live GNSS continuation cancellation/re-anchor orchestration
for an authenticated mutation remains future writer-slice work.

## 6. Compatibility / non-impact

M6D3C does not change:

- TLP v1 bytes;
- RF settings or relay behavior;
- GNSS freshness/acquisition rules;
- M6D2 3-observation / 2-of-3 confirmation;
- HistoryStore format;
- ConfigStore format;
- SecurityStore format;
- BLE bond storage;
- ORUN application GATT bytes;
- secure-command or EVENT protocol.

The temporary M6D3B source-contract rule forbidding a production GeofenceStore
instance is intentionally superseded here. All M6D3B persistence-owner,
preflight and test-image isolation checks remain in force.

## 7. Tests added/updated

### Pure provider

`test_m6d3c_geofence_runtime_provider.cpp` covers:

- CONFIGURED snapshot -> runtime geometry;
- multiple durable polygons reconstructed from flattened storage;
- CLEAR -> runtime unconfigured/base cadence;
- geometry replacement resets old confirmation evidence;
- no synthetic transition occurrence from replacement;
- malformed structure rejected without replacing an existing valid runtime.

### Source contract

`test_m6d3c_source_contract.py` verifies:

- exactly one production GeofenceStore;
- it uses the shared FlashMutationGate geofence port;
- recovery occurs before Bluefruit/SoftDevice;
- production consumes only currentSnapshot;
- no production requestReplace/requestClear writer is enabled;
- provider owns no GNSS/radio/BLE/flash/transport responsibility;
- semantic snapshot application is not gated on token VALID.

### Production startup composition

The host startup harness now maps the real geofence partition and covers:

- blank partition -> committed CLEAR baseline -> M6D2 remains unconfigured;
- pre-seeded committed CONFIGURED record -> setup recovery -> real production
  provider configures M6D2 before GNSS service;
- existing startup/radio/BLE/history behavior remains exercised.

## 7.1. Host validation evidence

Complete `./tests/run_host_tests.sh` execution: **PASS**.

New/affected evidence includes:

- `M6D3C durable snapshot -> M6D2 runtime provider checks: PASS`;
- `M6D3C durable geofence runtime activation contract: PASS`;
- `Production startup identity/history/loop (geofence_persisted): PASS`;
- existing M6D3B GeofenceStore / flash-gate suites remain PASS;
- all pre-existing B1A..B4, M3..M7, R2..R4 and production startup scenarios
  remain PASS.

The host harness uses warnings-as-errors plus ASan/UBSan for the applicable
targets. This is software evidence only; no M6D3C physical runtime activation
has been claimed.

## 8. Validation gates

Required before merge:

```text
focused M6D3C host/source tests
-> complete host suite + warnings/ASan/UBSan
-> production RAK4630 build + RAM/Flash
-> independent Astra audit
-> fixes + affected/full retest
-> focused physical boot/recovery test if audit requires it
-> final disposition
-> merge
```

A build PASS is not physical proof.

## 9. Physical evidence boundary

M6D3B already physically proved the durable store itself:

- fresh baseline;
- A/B successor and rollover;
- reboot persistence;
- focused electrical cut before commit.

M6D3C changes production composition: a recovered CONFIGURED snapshot now
affects the actual local M6D2 runtime. That integration has not yet been
physically executed in this milestone.

Do not claim physical M6D3C runtime activation until a dedicated physical
observation is recorded.
