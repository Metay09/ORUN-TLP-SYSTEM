# Pre-M6 B3 — Legacy Role Compatibility Mapping

## Baseline

B3 is stacked on B2 commit `d62b76815118632a2eebb263b7924b57812b7dd2`.
B2 remains subject to its independent final audit/merge gate. B3 must therefore
not be merged independently if B2 is not accepted.

The frozen M0–M5/R1–R4 behavior, B1A compatibility fixtures and B1B physical
GNSS→POSITION→BASE evidence remain the behavioral reference.

Current concept/ownership invariants are also recorded in
`docs/architecture/ORUN_CURRENT_ARCHITECTURE_RULES.md` so legacy role behavior is
not mistaken for the target product model.

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

**Invariant:** relay forwarding must not disable application services. A node may
originate its own sensor/position/event traffic and independently forward other
eligible nodes' traffic. The animal-tracker preset will default forwarding OFF
for battery/airtime reasons; that default is not a permanent type restriction.

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

`firmware/tests/b3/test_b3.cpp` compiles under `gnu++11` with warnings-as-errors
and sanitizers in the host runner, matching the language level that exposed the
RAK4630 toolchain regression. It freezes:

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

## Validation evidence

Code-bearing branch head validated by the owner:

`a9d7bde7afcce2d7a34912c6fc4cdfcbf1788986`

- `./firmware/tests/run_host_tests.sh`: PASS for B1A, B2, B3, M3, R3, M4,
  M5, R2.1, identity fixtures, R2, startup scenarios and R4 guards;
- `pio run -e rak4630`: SUCCESS;
- build usage: RAM 13,852 / 248,832 bytes (5.6%), flash 139,576 / 815,104
  bytes (17.1%);
- DFU upload: SUCCESS to a RAK4631;
- runtime USB path: `ROLE?` returned `ROLE BASE mode=AUTO` for that boot, and
  `ROLE TRACKER` returned `ROLE TRACKER source=USB-OVERRIDE`.

The monitor was attached after boot, so the corresponding `GNSS: detected` or
`GNSS: not detected` boot line was not captured. `ROLE BASE mode=AUTO` must not
be interpreted as an indoor satellite-fix failure: current AUTO is driven by
GNSS hardware detection, not by open-sky fix acquisition.

B1B previously demonstrated the real open-sky GNSS -> POSITION -> Base DIRECT
chain and remains the physical reference. The exact B2+B3 code-bearing commit
has not yet repeated that physical chain. Host/build/upload evidence is not a
replacement for that regression or for the pending independent final audit.

This documentation-only follow-up does not change firmware bytes and therefore
does not require another PlatformIO build by itself.

## Next bounded work

After the stacked B2+B3 final audit/merge gate, the next architecture step is a
typed configuration/command boundary so relay forwarding can eventually become
an explicit user-controlled setting without coupling it to application profile.
The same boundary should establish capability presence as independent from
service enablement; GNSS is the first existing hardware case, not permission to
build a speculative general sensor framework.

Persistent config still waits for verified partition/ownership work described by
G07/G08; do not use history pages as an ad-hoc config database.

Do not implement multi-hop in that configuration change. Multi-hop belongs to a
separate reviewed network/protocol milestone after its forwarding envelope,
dedupe identity, hop/flood policy, airtime budget, security model and mixed-fleet
behavior are specified.
