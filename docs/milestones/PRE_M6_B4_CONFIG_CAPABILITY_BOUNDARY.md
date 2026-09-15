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

## Phase 1 implemented

The first B4 slice adds a portable C++11-compatible pure model:

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

The model deliberately preserves requested intent when a capability cannot
currently satisfy it. Example: tracking requested with GNSS absent resolves to a
blocked effective tracking state; it does not rewrite tracking requested OFF.

## Current compatibility mapping into B4

| Legacy role | Tracking requested | Relay forwarding requested | Location source |
| --- | --- | --- | --- |
| TRACKER | ON | OFF | GNSS |
| RELAY | OFF | ON | NONE |
| BASE | OFF | OFF | NONE |

This table is a migration/compatibility adapter only. BASE application reception
remains legacy behavior and is intentionally not frozen into the new public
configuration schema.

## Runtime impact of Phase 1

None yet. Production `main.cpp`, `RadioManager` and `NetworkService` still execute
the existing B2/B3 runtime paths. In particular, actual relay forwarding remains
owned by the installed legacy `NodeRole` inside `NetworkService` until the next
bounded B4 slice moves that ownership to resolved/effective behavior while
preserving M5 queue, dedupe, timing and role-transition safety.

This separation is intentional: first freeze the pure semantics with host tests,
then perform the behavior-preserving production wiring as a separately reviewable
change.

## Compatibility impact

Phase 1 changes no:

- TLP v1 bytes or packet sizes;
- POSITION/RELAY_FORWARD encoding;
- RF parameters or airtime;
- GNSS acquisition/freshness state machine;
- storage/journal layout;
- device identity or sequence semantics;
- flash ownership;
- power/sleep behavior;
- USB ROLE command behavior.

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

The B4 test is added to `firmware/tests/run_host_tests.sh` under `gnu++11`,
matching the current RAK4630 compiler language constraint that previously exposed
a B3 compatibility issue.

## Validation status

Not yet claimed for this B4 branch. Required before calling Phase 1 PASS:

1. full `./firmware/tests/run_host_tests.sh`;
2. `pio run -e rak4630` because new firmware translation units are compiled into
   the production image even though they are not yet wired into behavior;
3. review compiler warnings and branch diff.

No physical hardware test is required for Phase 1 alone because it has no runtime
wiring. A later B4 slice that changes actual forwarding/config behavior must be
assessed separately for physical regression needs.

## Next bounded slice

After Phase 1 host/build PASS, move actual relay forwarding ownership away from
raw `NodeRole` toward explicit resolved/effective network behavior without:

- exposing a user-facing relay toggle yet;
- changing TLP v1;
- changing one-hop behavior;
- changing relay queue/dedupe/timing;
- changing BASE legacy receive semantics;
- rewriting the existing radio role-transition state machine.
