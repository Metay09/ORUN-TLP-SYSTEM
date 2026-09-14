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
   independent forwarding and current POSITION-service behavior;
2. keep AUTO GNSS detection as a legacy bootstrap heuristic only;
3. route the production tracker POSITION admission decision through that
   compatibility mapping instead of branching directly on the role enum.

B3 does **not** replace the legacy role enum, add persistent configuration,
change USB commands, add profiles/capability discovery, introduce BLE/PHONE
location, or rewrite RadioManager/NetworkService role-transition state machines.
Those remain separate bounded changes.

## Frozen compatibility mapping

| Legacy role | Forwarding responsibility | Publish own GNSS POSITION | Receive application POSITION |
| --- | --- | --- | --- |
| TRACKER | END_NODE | yes | no |
| RELAY | RELAY | no | no |
| BASE | END_NODE | no | yes |

`ForwardingResponsibility` is deliberately small: only `END_NODE` and `RELAY`
exist because those are the responsibilities already implemented and physically
relevant today. Gateway bridging, profile, capability, location source and power
policy are not inferred from this value.

`LegacyRoleBehavior` is a compatibility projection, not a persisted config
schema. It exists to stop new code from treating `NodeRole` as the permanent
owner of unrelated concepts.

## AUTO compatibility

`RoleController::updateAutomatic` is intentionally unchanged:

- completed GNSS detection + GNSS present -> TRACKER;
- completed GNSS detection + GNSS absent -> BASE;
- an explicit USB override disables AUTO until reboot.

This is preserved only because it is existing behavior. It must not become the
future rule that hardware presence determines application/profile or forwarding
responsibility. A later validated configuration boundary will take precedence
over this unprovisioned bootstrap behavior.

## Production effect

`main.cpp` now obtains `LegacyRoleBehavior` and uses
`publish_gnss_position` for the same gate that previously tested
`role == TRACKER` directly. The mapping makes the dependency explicit while
keeping the boolean result identical for every existing role.

Radio role-transition quiescence, relay/base handling, GNSS polling/detection,
storage, sequence allocation, wire bytes, RF parameters and power/recovery state
machines are untouched.

## Tests

`firmware/tests/b3/test_b3.cpp` compiles with the portable host flags and no
Arduino/SparkFun/Nordic/SX126x stubs. It freezes:

- TRACKER -> END_NODE + publish + no application receive;
- RELAY -> RELAY + no publish + no application receive;
- BASE -> END_NODE + no publish + application receive;
- the existing AUTO and explicit-override behavior.

The existing M5/R2/startup suites continue to own actual relay/base radio role
semantics and role-transition safety.

## Compatibility impact

- TLP v1 wire bytes: none;
- POSITION size/codec: none;
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

The new B3 portable test must report:

```text
B3 legacy role compatibility mapping: PASS
```

A new dedicated physical test is not required for B3 because it changes no RF,
GNSS, storage or hardware-driver behavior. The still-open B2 final physical/audit
gates remain applicable to the stacked branch before merge.

## Next bounded work

After B3 validation, the next architecture step should use this compatibility
mapping to establish the typed configuration/command boundary without allocating
persistent flash yet. Durable configuration remains blocked on the separately
verified partition/ownership work described by G07/G08. Do not use history pages
as an ad-hoc config database.
