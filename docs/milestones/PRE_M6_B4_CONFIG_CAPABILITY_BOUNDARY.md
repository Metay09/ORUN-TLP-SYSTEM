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
- relay resolution remains independent when tracking is blocked;
- invalid tracking configuration is blocked explicitly.

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

## Validation status

Owner-run evidence for the current Phase 1–3 implementation (code-bearing
composition commit `79715c9e1e3440823016b278e828cc1b38d6bc3d`, with startup host
link closure at `6d568c4f0924c6db12b459ea59c9b648a72cd6be`):

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

The Phase 3 build increases flash by 640 bytes versus the preceding B4 runtime
seam build (139,528 -> 140,168 bytes) and does not increase RAM.

No physical validation is claimed for B4 yet. Phase 2/3 alter runtime
ownership/application gating even though the legacy role-visible behavior is
intended to remain identical. Prior B2/B3 GNSS->POSITION->Base evidence is not
re-labeled as B4 hardware validation.

The smallest useful B4 physical regression is a mixed-fleet-compatible direct
path check: run the current B4 image on the tracker while leaving the previously
validated Base image unchanged, confirm AUTO/override resolves TRACKER after GNSS
detection, obtain a real fresh GNSS fix, and verify the unchanged Base receives
the normal DIRECT POSITION. This exercises capability resolution -> effective
tracking -> store-before-send -> frozen TLP v1 -> old Base reception without
requiring a new user-facing relay toggle.

A separate relay-path hardware check may be added if final review finds it
necessary; the host suite already exercises the independent TRACKER+relay runtime
seam and legacy M5 relay behavior, but host evidence is not physical RF evidence.

## Remaining bounded work

Before B4 closure:

1. run the smallest current-image physical regression described above;
2. review the complete branch diff and affected architecture documentation;
3. run independent Astra audit later, as requested by the owner;
4. fix any findings and repeat affected validation before merge.

B4 still must **not** add durable config, BLE, a generic capability registry,
multi-hop, a new protocol, backend/mobile work or speculative hardware support.
