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

Owner-run evidence through commit `108724e4fc5ec385c20182b7bbc6104cc5cc9d96`:

- full `./firmware/tests/run_host_tests.sh`: PASS;
- B4 pure config model: PASS;
- independent NetworkService relay forwarding seam: PASS;
- RadioManager independent relay behavior apply: PASS;
- existing B1A/B2/B3/M3/R3/M4/M5/R2/R4/startup regressions: PASS;
- `pio run -e rak4630`: SUCCESS;
- RAM: 13,852 / 248,832 bytes = 5.6%;
- Flash: 139,528 / 815,104 bytes = 17.1%;
- observed build warnings remain inside pinned SX126x-Arduino third-party sources.

The Phase 3 `main.cpp` composition-root wiring was added after that evidence and
must still run the full host suite and RAK4630 build before it is claimed PASS.

No physical validation is claimed for B4 yet. Phase 1 had no runtime effect, but
Phase 2/3 do alter runtime ownership/application paths even though legacy behavior
is intended to remain identical. Physical regression need will be decided after
host/build closure and final review; prior B2/B3 GNSS->POSITION->Base evidence is
not automatically re-labeled as B4 hardware validation.

## Remaining bounded work

Before B4 closure:

1. validate the composition-root wiring with the full host suite;
2. rebuild RAK4630 and review RAM/flash/warnings;
3. review branch diff and affected architecture documentation;
4. decide the smallest physical regression needed for the runtime ownership
   change;
5. run independent Astra audit later, as requested by the owner;
6. fix any findings and repeat affected validation before merge.

B4 still must **not** add durable config, BLE, a generic capability registry,
multi-hop, a new protocol, backend/mobile work or speculative hardware support.
