# M6D3C — Durable geofence snapshot -> M6D2 runtime provider

Status: **COMPLETE; FINAL HOST + PRODUCTION RAK4630 BUILD PASS; INDEPENDENT ASTRA AUDIT PASS; POST-AUDIT COMPLETE HOST RETEST PASS; PHYSICAL RUNTIME ACTIVATION PASS; MERGE READY**.

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

## 7.4. Independent Astra audit

Independent final audit at branch head
`8b6342264e1d073be8fd6d010a42816928f9da8a`: **PASS**.

Severity summary:

- BLOCKER: 0
- HIGH: 0
- MEDIUM: 0
- LOW: 3

Accepted LOW follow-ups:

1. add a real production-startup regression proving a readable CONFIGURED
   snapshot remains active when GeofenceTokenState is UNCERTAIN;
2. clarify that the legacy M6D2 physical probe now inherits durable M6D3C boot
   activation and therefore requires a CLEAR durable resource to exercise its
   temporary first-fix fixture;
3. record the existing M6D3B fail-closed residual: a power cut while creating
   the very first blank-partition CLEAR baseline can leave the resource
   UNAVAILABLE, and production M6D3C intentionally has no maintenance/writer
   recovery path yet.

The audit independently re-ran the complete host suite and production RAK4630
build and measured the same final footprint (28,744 B RAM / 260,008 B Flash).
It also used `-fstack-usage` and measured the deepest first-baseline failure
path at approximately 2.78 KiB of the 4-KiB loop-task stack, consistent with
the prior M6D3B stack qualification.

No production runtime bug or compatibility regression was found.

## 7.5. Residual first-boot baseline recovery limit

M6D3C production now calls `GeofenceStore::begin()` on every boot. On a truly
blank geofence partition, M6D3B's frozen contract establishes an explicit
committed CLEAR baseline rather than interpreting erased flash as CLEAR.

If electrical power is lost during that **first baseline creation**, partially
programmed/torn evidence can remain. M6D3B intentionally fails closed:
the resource may become UNAVAILABLE/maintenance-required rather than guessing
CLEAR. M6D3C does not add a production maintenance/reset writer, so such a unit
cannot self-heal that evidence in this slice.

This is a known residual, not a semantic safety failure: the device does not
invent a geofence or emit a false OUTSIDE event. The future authenticated
geofence writer/maintenance slice must provide an explicit recovery path for
this state.

## 7.6. Post-audit retest

After applying the accepted LOW audit follow-ups, the complete host suite was
repeated at branch head `a1f9a5525512f33a1069d0f42507fc080290032c`: **PASS**.

The final startup matrix now additionally includes:

- `Production startup identity/history/loop (geofence_uncertain): PASS`.

This proves the production composition continues to apply a readable
CONFIGURED semantic snapshot to M6D2 even when the durable mutation/CAS token
is UNCERTAIN, while leaving the recovery path read-only.

No production firmware source changed after the independently audited runtime
head; the post-audit delta is limited to host regression coverage,
test-environment documentation and milestone documentation. Therefore the
already-recorded post-stack-hardening RAK4630 production build evidence remains
applicable:

- RAM: **28,744 / 248,832 B (11.6%)**;
- Flash: **260,008 / 815,104 B (31.9%)**;
- application-ceiling guard: **PASS**;
- exclusive-owner guard: **PASS**.

The software/audit gate is now closed. Physical M6D3C runtime activation is the
remaining pre-merge evidence.

## 7.6. Post-audit full host retest

After accepting the audit's LOW L1 regression recommendation and documentation
follow-ups, the complete host suite was repeated.

Final post-audit `./tests/run_host_tests.sh`: **PASS**.

The new production-composition regression also passes:

- `Production startup identity/history/loop (geofence_uncertain): PASS`.

This proves the intended M6D3B/M6D3C semantic split is regression-guarded:
a readable CONFIGURED snapshot remains active in M6D2 even when mutation/CAS
token authority is UNCERTAIN.

No production runtime source changed after the previously successful RAK4630
build, so the 28,744 B RAM / 260,008 B Flash production build evidence remains
applicable.

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

## 9. Physical runtime qualification

M6D3B had already physically qualified the durable store itself: fresh
baseline, A/B successor and rollover, reboot persistence, and the focused
electrical cut before commit.

M6D3C production runtime activation was then physically exercised on development
unit `ID_SERIAL_SHORT=0E8ADE7E71531AA3` with the previously-qualified durable
CONFIGURED resource:

- incarnation: `0xD93BBFF182C898DC`;
- active revision/generation: `4 / 4`;
- snapshot: `CONFIGURED`, one area, three effective vertices.

Observed production behavior:

- the first valid outdoor episode produced the required three accepted GNSS
  observations before the coordinator settled its initial classification;
- no synthetic `GEOFENCE OUTSIDE confirmed` transition was emitted during
  that initial classification;
- with the persisted CONFIGURED resource active, the runtime cadence changed
  from base `B=180 s` to the existing M6D2 `B/3` cadence, observed as
  approximately 60-second acquisition starts;
- after a device reset, the same approximately 60-second cadence resumed,
  providing behavioral evidence that the persisted CONFIGURED snapshot was
  recovered and re-applied to the M6D2 runtime.

The exact early-boot log
`GEOFENCE runtime configured areas=... vertices=... token=...` was **not**
physically observed because USB CDC disconnected/re-enumerated across reset.
This milestone therefore does not claim that exact log line as observed
evidence; the runtime behavior above is the physical activation evidence.

A final read-only M6D3B qualification image was then flashed to verify that
production had not mutated the durable geofence resource. Its STATUS output was:

```text
M6D3B QUAL STORE ready=yes busy=no maintenance=no resource=CONFIGURED token_state=VALID mutations=0 failures=0 reconciliations=0
M6D3B QUAL TOKEN incarnation=0xD93BBFF182C898DC revision=4
M6D3B QUAL SNAPSHOT state=CONFIGURED areas=1 vertices=3
M6D3B QUAL PAGE A evidence=COMMITTED_CLEAR decoded=yes tail_ff=yes generation=0x0000000000000003 incarnation=0xD93BBFF182C898DC revision=3 state=CLEAR areas=0 vertices=0
M6D3B QUAL PAGE B evidence=COMMITTED_CONFIGURED decoded=yes tail_ff=yes generation=0x0000000000000004 incarnation=0xD93BBFF182C898DC revision=4 state=CONFIGURED areas=1 vertices=3
```

This proves the production M6D3C path remained read-only with respect to
GeofenceStore while consuming the durable semantic snapshot.

After the qualification check, the normal production `rak4630` image was
restored successfully:

- RAM: **28,744 / 248,832 B (11.6%)**;
- Flash: **260,008 / 815,104 B (31.9%)**;
- upload: **SUCCESS**.

Physical qualification scope is intentionally limited to this integration
slice. It does not turn host/build evidence into RF, GNSS accuracy, power,
security, BLE-write or backend qualification.


## 10. Product follow-up after M6D3C

The physical M6D3C workflow exposed a product-level serviceability gap: once a
device leaves the USB-connected bench, production runtime state such as GNSS and
geofence status is not yet available through the phone-facing BLE application
surface.

This does not change M6D3C scope. After M6D3C is closed, prioritize a small
read-only Device Service / Diagnostics slice before adding unrelated new RF or
backend feature work.

Required direction:

- USB and BLE must converge on the same typed application/service owners;
- BLE is intended for local setup, settings and service/diagnostics, not merely
  GATT transport proof;
- first expose bounded read-only device/location/GNSS/geofence/power/radio/
  storage/health state where ownership is already defined;
- do not stream raw Serial logs as the product API;
- protected config/geofence mutation remains separately gated on reviewed
  authentication, authorization, anti-replay and command/idempotency rules.

This is a product completion requirement, not authorization to expand the
current M6D3C runtime.
