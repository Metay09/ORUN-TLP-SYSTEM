# M6D3C — Durable geofence snapshot -> M6D2 runtime provider

Status: **IMPLEMENTATION COMPLETE; FINAL COMPLETE HOST + PRODUCTION RAK4630 BUILD PASS; INDEPENDENT ASTRA AUDIT / PHYSICAL RUNTIME ACTIVATION PENDING**.

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

## 7.2. Production RAK4630 build evidence

`pio run -e rak4630`: **SUCCESS**.

Measured production image:

- RAM before stack-hardening: **28,224 / 248,832 B (11.3%)**;
- RAM after moving the boot snapshot to static storage: **28,744 / 248,832 B (11.6%)**;
- Flash: **260,008 / 815,104 B (31.9%)**.

Relative to the M6D3B production baseline (24,224 B RAM / 252,748 B Flash),
final M6D3C adds **4,520 B RAM** and **7,260 B Flash**. The RAM increase is expected
because production now instantiates GeofenceStore's fixed recovery/verification
workspaces and durable snapshot state rather than leaving that owner absent from
the composition root.

The build log contains the existing pinned SX126x-Arduino dependency warnings
(`#warning USING RAK4630` and RAK11300 SimpleTimer signed/unsigned warnings);
no warning was emitted from M6D3C project sources.

Application-ceiling and exclusive-owner post-link guards completed successfully.

Post-build review then found one stack-margin hardening opportunity: the
record-sized boot snapshot copy was a `setup()` local. M6D3B had already
measured a tight-but-safe 4-KiB loop-task stack margin around
`GeofenceStore::begin()`, so M6D3C moved this boot scratch to static storage
rather than rely on compiler lifetime allocation. This is a RAM-placement
hardening change, not a semantic behavior change. The production RAK4630 build was repeated after that change and remained
SUCCESS at 28,744 B RAM / 260,008 B Flash. The complete host suite must be
repeated after the source-contract name fix before the host gate is final.

This is build evidence only, not physical M6D3C runtime evidence.

## 7.3. Final post-hardening host evidence

After moving the boot snapshot to static storage and repairing the source-contract
guard to follow the renamed `geofence_boot_snapshot`, the complete host suite
was repeated.

Final complete `./tests/run_host_tests.sh`: **PASS**.

Relevant final lines include:

- `M6D3C durable snapshot -> M6D2 runtime provider checks: PASS`;
- `M6D3C durable geofence runtime activation contract: PASS`;
- `Production startup identity/history/loop (geofence_persisted): PASS`;
- all prior production startup scenarios remain PASS;
- all M6D3B GeofenceStore/FlashMutationGate regression suites remain PASS;
- all existing B1A..B4, M3..M7 and R2..R4 suites remain PASS.

Final validated branch head at this software gate:
`bfc691dec8a7bb1b5bb447175f665c02c3639f66`.

Production RAK4630 build after the stack-hardening change remains:

- RAM: **28,744 / 248,832 B (11.6%)**;
- Flash: **260,008 / 815,104 B (31.9%)**;
- application-ceiling guard: **PASS**;
- exclusive-owner guard: **PASS**.

No physical M6D3C runtime activation is claimed yet.

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
