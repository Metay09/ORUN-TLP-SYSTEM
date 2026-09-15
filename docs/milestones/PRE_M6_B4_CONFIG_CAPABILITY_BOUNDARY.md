# Pre-M6 B4 — Runtime Configuration and Capability Boundary

Status: **IN PROGRESS**.
Branch: `refactor/pre-m6-b4-config-capability-boundary`.
Baseline: B2/B3 stack plus documentation/architecture review closure.

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
invalid cross-field combination cannot partially enable another service.

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

`NetworkService` now owns an explicit `relay_forwarding_enabled` runtime state.
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

`main.cpp` now derives a small GNSS `CapabilitySnapshot` from the existing bounded
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
  partially applying one field.

`firmware/tests/b4/test_b4_network.cpp` freezes independent NetworkService relay
forwarding behavior, including TRACKER + forwarding ON without changing the role.

`firmware/tests/b4/test_b4_radio.cpp` exercises the real RadioManager/R2 fake-radio
boundary and verifies:

- TRACKER + relay forwarding ON queues and transmits RELAY_FORWARD;
- disabling forwarding cannot abort/reinterpret an in-flight relay TX;
- disable applies after TX completion;
- new packets are no longer admitted for forwarding after disable;
- cumulative relay diagnostics remain cumulative rather than masquerading as
  instantaneous queue depth.

The B4 pure model remains compiled under `gnu++11`, matching the current RAK4630
compiler language constraint. Radio/network tests retain host warnings as errors
and ASan/UBSan coverage through the normal host suite.

## Complete branch review

The complete B4 diff from baseline
`28d254d1f40710fcedd64deab9ce6c216c8d8992` was reviewed through code head
`3186c96d54f0f84243296a776a0f1ffa5ed4c526`.

The review found one pre-merge semantic defect: an invalid RequestedConfig could
previously block tracking while still enabling relay forwarding. That contradicted
whole-candidate validation. It was fixed in:

- `489386ec8efc074191d7ac052de104b351d9996d` — fail-closed invalid-candidate
  resolution;
- `3186c96d54f0f84243296a776a0f1ffa5ed4c526` — regression coverage.

No P0/Critical blocker was found. Accepted bounded limitations and the detailed
compatibility/ownership review are recorded in
`docs/audits/PRE_M6_B4_BRANCH_REVIEW.md`.

Because those two post-review commits change source/test code after the earlier
host/build evidence below, the full host suite and RAK build must be rerun before
B4 is called revalidated.

## Validation status

Owner-run evidence for the Phase 1–3 implementation before the final branch-review
fix (code-bearing composition commit
`79715c9e1e3440823016b278e828cc1b38d6bc3d`, with startup host link closure at
`6d568c4f0924c6db12b459ea59c9b648a72cd6be`):

- full `./firmware/tests/run_host_tests.sh`: PASS;
- B4 pure config model: PASS;
- independent NetworkService relay forwarding seam: PASS;
- RadioManager independent relay behavior apply: PASS;
- production startup identity/history/loop scenarios with the Phase 3
  composition path: PASS;
- existing B1A/B2/B3/M3/R3/M4/M5/R2/R4 regressions: PASS;
- host builds retain `-Wall -Wextra -Werror` plus ASan/UBSan on the normal
  covered targets;
- `pio run -e rak4630`: SUCCESS on Nordic nRF52 platform 11.0.0 / GCC 7.2.1;
- RAM: 13,852 / 248,832 bytes = 5.6%;
- Flash: 140,168 / 815,104 bytes = 17.2%;
- the shown incremental Phase 3 build produced no B4-source warning; prior clean
  builds still contain only the known pinned SX126x-Arduino third-party warnings.

The Phase 3 build increased flash by 640 bytes versus the preceding B4 runtime
seam build (139,528 -> 140,168 bytes) and did not increase RAM. These size values
must be re-measured after the final B4-R1 fix; they are not yet claimed for the
post-review code head.

### Physical mixed-fleet direct regression — PASS

The owner physically validated the smallest B4 mixed-fleet regression with the
B4 composition image on the tracker and the previously validated Base image left
unchanged. This physical run occurred before the later pure invalid-candidate
B4-R1 fix:

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

The B4-R1 fix affects only semantically invalid RequestedConfig candidates. The
current production RequestedConfig source is the frozen legacy mapping, whose
TRACKER/RELAY/BASE projections are all valid, so B4-R1 is not reachable in the
current hardware path. A repeat GNSS/RF hardware run is therefore not required
solely for that fix; host and RAK build revalidation are required. This decision
must be revisited when a mutable configuration source is introduced.

This PASS does **not** prove flash power-cut recovery/readback, long-range RF,
current consumption, multi-hop, relay coexistence, or an independently configured
TRACKER+relay hardware path. The latter remains host-tested only because B4 does
not yet expose a user-facing independent relay toggle.

A separate relay-path hardware check is not required for B4 closure solely from
this review; the host suite exercises the independent TRACKER+relay runtime seam
and legacy M5 relay behavior. Host evidence is still not physical RF evidence.

## Remaining bounded work

Before B4 closure:

1. rerun the full host suite after the B4-R1 review fix;
2. rerun `pio run -e rak4630` and record final RAM/flash/warnings;
3. run independent Astra audit later, as requested by the owner;
4. fix any Astra findings and repeat affected validation before merge.

B4 still must **not** add durable config, BLE, a generic capability registry,
multi-hop, a new protocol, backend/mobile work or speculative hardware support.
