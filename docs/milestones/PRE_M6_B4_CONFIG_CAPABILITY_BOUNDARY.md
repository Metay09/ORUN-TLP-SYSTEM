# Pre-M6 B4 — Runtime Configuration and Capability Boundary

Status: **READY FOR MERGE**.
Branch: `refactor/pre-m6-b4-config-capability-boundary`.
Baseline: B2/B3 stack plus documentation/architecture review closure.
Post-Astra validated code/test head: `6d6f2dc756263020bda6596a10459b7d708d22f3`.

## Purpose

B4 introduces the smallest runtime-only seam needed before M6 so requested user
intent, detected hardware capability and effective service state do not collapse
back into the legacy TRACKER/RELAY/BASE role enum.

This milestone does not authorize durable configuration, BLE, a generic sensor
registry, a new location data model, multi-hop, TLP v2, backend/mobile work or
security/command implementation.

## Phase 1 — pure requested/capability/effective model

B4 adds a portable C++11-compatible pure model:

- `RequestedConfig`
  - tracking requested on/off;
  - relay forwarding requested on/off;
  - current requested location source limited to `NONE` or `GNSS`;
- `CapabilitySnapshot`
  - GNSS support;
  - GNSS presence: UNKNOWN / PRESENT / ABSENT;
  - GNSS health: OK / DEGRADED / FAULT / UNAVAILABLE;
- `EffectiveConfig`
  - service state: DISABLED / ENABLED / BLOCKED / DEGRADED;
  - explicit reason;
- pure whole-candidate validation;
- pure requested + capability -> effective resolution;
- compatibility mapping from `LegacyRoleBehavior` into requested tracking/relay
  intent without promoting `receive_application_position` into public config.

The model preserves requested intent when a capability cannot currently satisfy
it. Example: tracking requested with GNSS absent resolves to a blocked effective
tracking state; it does not rewrite tracking requested OFF.

Invalid requested candidates are atomic. A future configuration owner must reject
an invalid candidate before replacing the prior requested state. The pure resolver
also fail-closes all effective services if an invalid candidate reaches it, so an
invalid cross-field combination or an undefined location-source value cannot
partially enable another service.

Defined requested location-source values are currently exactly:

```text
NONE
GNSS
```

Any enum value outside that domain is rejected as `kInvalidLocationSource`, even
when tracking itself is disabled.

## Current compatibility mapping into B4

| Legacy role | Tracking requested | Relay forwarding requested | Location source |
| --- | --- | --- | --- |
| TRACKER | ON | OFF | GNSS |
| RELAY | OFF | ON | NONE |
| BASE | OFF | OFF | NONE |

This table is a migration/compatibility adapter only. BASE application reception
remains legacy behavior and is intentionally not frozen into the new public
configuration schema.

## Phase 2 — independent forwarding runtime ownership

`NetworkService` owns an explicit `relay_forwarding_enabled` runtime state.
Forwarding admission, nested-relay rejection, due-forward extraction and relay
diagnostics use that state instead of treating `NodeRole::kRelay` as the service
itself.

Legacy `setRole()` still installs the historical default so TRACKER/RELAY/BASE
behavior remains byte/runtime compatible. BASE application reception remains
role-based compatibility behavior and is not promoted into public configuration.

`RadioManager` schedules relay TX from the explicit forwarding state rather than
`network_.role() == RELAY`.

A bounded `RadioManager::setRelayForwardingEnabled()` apply path preserves radio
ownership and transition invariants:

- no behavior change is applied during an in-flight TX;
- the driver gate remains the single radio owner;
- pending dependency work is consumed before transition;
- already-handed-off RX is drained under the old behavior;
- radio IRQ state is quiesced before the new behavior is installed;
- the receive epoch is advanced so delayed old callbacks cannot be reinterpreted;
- RX is restored afterward;
- callers can retry a deferred apply on a later cooperative loop pass.

This makes TRACKER + relay forwarding ON representable and host-testable without
introducing a user-facing toggle or persistence.

A current compatibility limitation is intentionally retained: legacy BASE
application reception and relay forwarding are not yet a defined dual-use mode.
If an internal BASE instance is forced to relay-forward, forwarding takes
precedence in `NetworkService::receive()`. B4 therefore does not authorize a
user-facing BASE/collector + relay combination; future collector/gateway/subscriber
semantics must define that coexistence explicitly before exposure.

## Phase 3 — composition-root wiring

`main.cpp` derives a small GNSS `CapabilitySnapshot` from the existing bounded
GNSS detection result and resolves the frozen legacy requested defaults through:

```text
legacy compatibility defaults
        -> RequestedConfig
        -> CapabilitySnapshot
        -> resolveRequestedConfig()
        -> EffectiveConfig
        -> PositionFlow / RadioManager
```

Important boundaries remain explicit:

- firmware support and physical GNSS presence are separate facts;
- `UNKNOWN` remains distinct from `ABSENT` until bounded detection completes;
- GNSS manager detection/acquisition/power ownership is unchanged;
- tracking effective state gates PositionFlow/fix admission;
- relay effective state is applied through the safe RadioManager behavior path;
- legacy role still owns the compatibility-only BASE receive behavior;
- USB `ROLE` commands and AUTO bootstrap remain unchanged compatibility surfaces.

The composition source is still the legacy role projection. There is no new
user-visible config surface yet. A later validated configuration source can
replace those requested defaults without returning runtime ownership to
`NodeRole`.

The current composition health projection is deliberately coarse: bounded GNSS
detection supplies UNKNOWN/PRESENT/ABSENT, while a detected device is currently
reported as `PRESENT + OK`. Acquisition timeout/recovery remains owned by the
existing GnssManager state machine and is not yet projected as a full product
health status. B4 must not be described as complete runtime health aggregation.

## Compatibility impact

B4 changes no:

- TLP v1 packet bytes or packet sizes;
- POSITION/RELAY_FORWARD encoding;
- one-hop/nested-relay policy;
- RF frequency, bandwidth, spreading factor, coding rate or TX power;
- relay queue size, dedupe keys or deterministic delay formula;
- GNSS acquisition/freshness state machine;
- GNSS power state machine;
- storage/journal layout or flash ownership;
- device identity or sequence semantics;
- USB ROLE command syntax or AUTO heuristic.

Relay forwarding ownership changes internally, but legacy observable role
behavior remains the same until a future explicit configuration surface is
authorized.

## Tests added

`firmware/tests/b4/test_b4.cpp` freezes:

- legacy TRACKER/RELAY/BASE -> requested-config compatibility mapping;
- tracking requires a requested location source;
- GNSS UNKNOWN is not ABSENT;
- PRESENT+DEGRADED is not ABSENT;
- unsupported/absent/fault/unavailable reasons remain distinct;
- requested intent is not mutated by resolution;
- tracking and relay can both be requested/effective simultaneously;
- relay resolution remains independent when tracking is capability-blocked;
- a valid relay-only request remains valid;
- an invalid whole candidate fail-closes both tracking and relay instead of
  partially applying one field;
- undefined `RequestedLocationSource` values are rejected by validation and
  fail-close both effective services.

`firmware/tests/b4/test_b4_network.cpp` freezes independent NetworkService relay
forwarding behavior, including TRACKER + forwarding ON without changing the role.

`firmware/tests/b4/test_b4_radio.cpp` exercises the real RadioManager/R2 fake-radio
boundary and verifies:

- TRACKER + relay forwarding ON queues and transmits RELAY_FORWARD;
- disabling forwarding cannot abort/reinterpret an in-flight relay TX;
- disable applies after TX completion;
- new packets are no longer admitted for forwarding after disable;
- cumulative relay diagnostics remain cumulative rather than masquerading as
  instantaneous queue depth;
- driver-gate contention defers apply without driver entry;
- already handed-off RX drains under the old behavior;
- pending old IRQ work is consumed before the behavior epoch advances;
- a delayed old wake cannot relabel retained payload after the transition;
- a pending legacy role transition defers explicit relay apply;
- RX is restored after transition;
- TRACKER + relay forwarding enabled still originates its own POSITION traffic.

The B4 pure model remains compiled under `gnu++11`, matching the current RAK4630
compiler language constraint. Radio/network tests retain host warnings as errors
and ASan/UBSan coverage through the normal host suite.

## Review and audit closure

The complete B4 diff from baseline
`28d254d1f40710fcedd64deab9ce6c216c8d8992` received an internal branch/code/
architecture review and then an independent Astra audit.

Internal review found B4-R1: a semantically invalid candidate could previously
block tracking while still enabling relay forwarding. That contradicted
whole-candidate validation and was corrected before the independent audit.

Astra then audited pre-fix head
`80d9fd557222b46b4fa3696df38c110e816184b2` and reported:

- no P0/P1 blocker;
- one P2 validation defect: undefined `RequestedLocationSource` enum values could
  pass `validateRequestedConfig()` and allow partial effective behavior;
- one P3 coverage gap: the new relay setter needed direct interleaving regression
  coverage for the existing R2 ownership invariants.

The P2 defect was fixed by:

- `64d80fd38e8390921a894199a6d581987610c872`;
- `ff8a53321fe184debff477e9e41e67e3134703e2`;
- `9b26a6ab10a559f12d23f712c3a7da082c322753`.

The P3 coverage gap was closed without production radio-state-machine changes by:

- `6d6f2dc756263020bda6596a10459b7d708d22f3`.

Detailed independent-audit evidence and closure are recorded in
`docs/audits/PRE_M6_B4_ASTRA_FINAL_AUDIT.md`.

No P0/P1 merge blocker remains known at the validated code/test head.

## Final validation status

### Post-Astra owner host/build revalidation — PASS

Full `./firmware/tests/run_host_tests.sh`: **PASS** after all Astra findings were
addressed. The run included:

- B1A legacy packet golden/malformed checks;
- B2 portable identity and legacy POSITION mapping;
- B3 legacy role compatibility mapping;
- B4 requested/capability/effective config model including undefined source
  rejection;
- B4 independent NetworkService relay forwarding seam;
- M3/R3/M4/M5 regressions;
- R2/R2.1 radio ownership and patch guards;
- expanded B4 RadioManager relay-apply interleaving coverage;
- B1A RAK identity conversion and serial fixtures;
- production startup identity/history/loop scenarios: mutex, gate, queue, lora,
  success;
- R4 bounded Wire/I2C/power/watchdog checks.

`pio run -e rak4630`: **SUCCESS** on Nordic nRF52 platform 11.0.0 / GCC 7.2.1.
The build verified/applied the pinned R4 Adafruit nRF52 Wire patch and R2.1
SX126x 2.0.32 driver-gate patch.

Final measured image after Astra fixes:

- RAM: `13,852 / 248,832` bytes = **5.6%**;
- Flash: `140,184 / 815,104` bytes = **17.2%**.

No new project warning was present in the owner-supplied final build output.

### Physical mixed-fleet direct regression — PASS

The owner physically validated the smallest B4 mixed-fleet regression with the
B4 composition image on the tracker and the previously validated Base image left
unchanged. This physical run predates the later pure validation fix and
additional host-only relay interleaving tests:

- Tracker B identity: `0E8ADE7E71531AA3`;
- Base A identity: `09A462BD4B275BA5`;
- B4 image uploaded to Tracker B through the RAK4630 `nrfutil` DFU path:
  PASS (`Device programmed.`);
- Tracker B runtime query after boot: `ROLE TRACKER mode=AUTO`;
- GNSS acquisition/low-power state machine remained active on hardware; an indoor
  acquisition timed out and released the switched 3V3 sensor rail as designed;
- Tracker B was then powered outdoors under open sky while Base A remained indoors
  connected to the PC;
- unchanged Base A received a new direct POSITION from Tracker B:
  `BASE RX NEW source=0E8ADE7E71531AA3 seq=4096 path=DIRECT rssi=-71 snr=9`.

Because the current tracker path only originates POSITION from admitted fresh GNSS
fixes and PositionFlow preserves store-before-send ordering, this physical result
exercises the B4 capability/effective-tracking composition through the normal
GNSS -> PositionFlow -> frozen TLP v1 -> RF -> legacy Base direct-receive path.
It also demonstrates the intended mixed-fleet compatibility for this direct path.

The post-hardware Astra P2 fix affects only semantically undefined configuration
values that current frozen legacy projections never produce. The P3 closure adds
host tests only. Therefore no repeat GNSS/RF hardware run is required solely for
these changes. This decision must be revisited when a mutable configuration
source is introduced.

This PASS does **not** prove flash power-cut recovery/readback, long-range RF,
current consumption, multi-hop, relay coexistence, or an independently configured
TRACKER+relay hardware path. The latter remains host-tested only because B4 does
not yet expose a user-facing independent relay toggle.

## Pre-merge impact report

```text
Code impact
- runtime-only requested/capability/effective boundary
- explicit relay-forwarding runtime ownership
- safe RadioManager forwarding apply seam
- undefined location-source values now rejected fail-closed

Architecture impact
- Role != Location Source != GNSS Power != Capability != Transport != Identity != Profile != User Identity preserved
- requested intent remains separate from effective state
- relay forwarding remains independent from application tracking

Protocol impact
- none; TLP v1 bytes/sizes/golden fixtures unchanged

Storage impact
- none; journal layout/ownership/store-before-send unchanged

Power/RF impact
- no RF PHY or airtime change
- no GNSS power-policy change
- relay availability semantics unchanged

Docs updated
- current architecture rules
- B4 branch review
- B4 milestone
- independent Astra final audit

Tests run
- full host suite PASS
- RAK4630 build SUCCESS
- expanded B4 validation/interleaving regressions PASS

Hardware validation status
- mixed-fleet direct GNSS -> store-before-send -> TLP v1 POSITION -> old Base PASS
- no claim for flash power-cut, long range, current, multi-hop, BASE+relay or independent TRACKER+relay hardware path
```

## Closure

B4 is **merge-ready** at validated code/test head
`6d6f2dc756263020bda6596a10459b7d708d22f3` plus documentation-only closure
commits. Any further source/runtime/test change must repeat the validation affected
by that change before merge.

B4 still must **not** add durable config, BLE, a generic capability registry,
multi-hop, a new protocol, backend/mobile work or speculative hardware support.
