# Pre-M6 B3 — Legacy Role Compatibility Mapping

## Baseline

B3 is stacked on B2 commit `d62b76815118632a2eebb263b7924b57812b7dd2`.
B2 remains subject to its independent final audit/merge gate. B3 must therefore
not be merged independently if B2 is not accepted.

The frozen M0–M5/R1–R4 behavior, B1A compatibility fixtures and B1B physical
GNSS→POSITION→BASE evidence remain the behavioral reference.

## Scope

B3 takes the smallest pre-M6 step toward architecture gaps G01 and G02:

1. make the current TRACKER/RELAY/BASE shorthand project explicitly onto
   independent current behavior values;
2. represent relay forwarding as an independent enablement value rather than a
   permanent node classification;
3. keep AUTO GNSS detection as a legacy bootstrap heuristic only;
4. route the production tracker POSITION admission decision through that
   compatibility mapping instead of branching directly on the role enum.

B3 does **not** replace the legacy role enum, expose a user-configurable relay
switch yet, add persistent configuration, change USB commands, add profiles or
capability discovery, introduce BLE/PHONE location, implement multi-hop, or
rewrite RadioManager/NetworkService role-transition state machines. Those remain
separate bounded changes.

## Frozen compatibility mapping

| Legacy role | Relay forwarding enabled | Publish own GNSS POSITION | Receive application POSITION |
| --- | --- | --- | --- |
| TRACKER | no | yes | no |
| RELAY | yes | no | no |
| BASE | no | no | yes |

`relay_forwarding_enabled` is deliberately a boolean behavior axis. It does not
mean that a node's application/profile is RELAY. Future validated configuration
may enable relay forwarding on a node that also runs tracking, telemetry,
sensing or actuation services, or disable forwarding on a node whose preset
normally enables it.

This is only a compatibility projection of today's behavior. B3 does not yet
provide that runtime configuration surface; it prevents the current legacy enum
from becoming the permanent architecture.

Gateway bridging, profile, capability, location source, GNSS power and system
power policy remain independent concepts and are not inferred from this value.

## Network evolution boundary

Current TLP v1 remains exactly as implemented and validated: POSITION is direct
or carried by the existing one-hop RELAY_FORWARD envelope, and nested relay
envelopes remain rejected. B3 does not change any wire byte or RF behavior.

The product architecture must not treat that v1 one-hop rule as a permanent
future limitation. A future network-envelope specification may support bounded
multi-hop forwarding with duplicate suppression and explicit forwarding policy.
That work requires separate protocol, airtime, security, mixed-fleet and field
validation and is intentionally outside B3.

## AUTO compatibility

`RoleController::updateAutomatic` is intentionally unchanged:

- completed GNSS detection + GNSS present -> TRACKER;
- completed GNSS detection + GNSS absent -> BASE;
- an explicit USB override disables AUTO until reboot.

This is preserved only because it is existing behavior. It must not become the
future rule that hardware presence determines application/profile or relay
forwarding. A later validated configuration boundary will take precedence over
this unprovisioned bootstrap behavior.

## Production effect

`main.cpp` obtains `LegacyRoleBehavior` and uses `publish_gnss_position` for the
same gate that previously tested `role == TRACKER` directly. The mapping makes
the dependency explicit while keeping the boolean result identical for every
existing role.

Radio role-transition quiescence, relay/base handling, GNSS polling/detection,
storage, sequence allocation, wire bytes, RF parameters and power/recovery state
machines are untouched.

## Tests

`firmware/tests/b3/test_b3.cpp` compiles with the portable host flags and no
Arduino/SparkFun/Nordic/SX126x stubs. It freezes:

- TRACKER -> relay forwarding off + publish + no application receive;
- RELAY -> relay forwarding on + no publish + no application receive;
- BASE -> relay forwarding off + no publish + application receive;
- the existing AUTO and explicit-override behavior.

The existing M5/R2/startup suites continue to own actual relay/base radio role
semantics and role-transition safety.

## Compatibility impact

- TLP v1 wire bytes: none;
- POSITION/RELAY_FORWARD codec and one-hop runtime policy: none;
- device identity: none;
- sequence/ticket behavior: none;
- journal/storage layout: none;
- RF configuration/airtime: none;
- GNSS state machine/freshness: none;
- radio callback ownership/quiescence: none;
- USB ROLE command syntax/output: none.

No physical-hardware behavior change is intended by B3 itself.

## Required validation

From the repository root on Debian:

```bash
./firmware/tests/run_host_tests.sh
(cd firmware && pio run -e rak4630)
```

The B3 portable test must report:

```text
B3 legacy role compatibility mapping: PASS
```

A new dedicated physical test is not required for B3 because it changes no RF,
GNSS, storage or hardware-driver behavior. The still-open B2 final physical/audit
gates remain applicable to the stacked branch before merge.

## Next bounded work

After B3 validation, the next architecture step should establish the typed
configuration/command boundary so relay forwarding can eventually become an
explicit user-controlled setting without coupling it to application profile.
Persistent config still waits for verified partition/ownership work described by
G07/G08; do not use history pages as an ad-hoc config database.

Do not implement multi-hop in that configuration change. Multi-hop belongs to a
separate reviewed network/protocol milestone after its forwarding envelope,
dedupe identity, hop/flood policy, airtime budget, security model and mixed-fleet
behavior are specified.
