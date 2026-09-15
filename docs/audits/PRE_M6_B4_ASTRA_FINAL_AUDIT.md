# Pre-M6 B4 Astra Final Audit

Date: 2026-09-15
Branch: `refactor/pre-m6-b4-config-capability-boundary`
Audit baseline: `28d254d1f40710fcedd64deab9ce6c216c8d8992`
Audited pre-fix head: `80d9fd557222b46b4fa3696df38c110e816184b2`
Post-audit code/test head: `6d6f2dc756263020bda6596a10459b7d708d22f3`
Status: **independent audit findings addressed; owner host/build revalidation PASS; merge-ready unless the branch changes again**.

## Independent verdict

Astra reported no P0/P1 blocker. It found one reproducible P2 validation defect and one P3 test-coverage gap. It did not recommend a state-machine rewrite, protocol change, persistence work, BLE work, new HAL/framework, new hardware support or multi-hop expansion.

The P2 finding meant the branch was not merge-ready at the audited head. Both findings were then addressed on the same branch and the affected/full validation was rerun.

## P2 finding — undefined location-source value passed whole-candidate validation

Before the fix, `validateRequestedConfig()` rejected `tracking=ON + source=NONE` but did not reject enum values outside the defined `RequestedLocationSource::{kNone,kGnss}` domain. A value such as:

```cpp
static_cast<RequestedLocationSource>(2)
```

could therefore return `ConfigValidation::kOk`; `resolveGnssTracking()` would later classify tracking as invalid while relay could still be enabled. That contradicted the whole-candidate validation contract.

### Fix

- `64d80fd38e8390921a894199a6d581987610c872` — adds the validation result needed for an invalid location-source domain.
- `ff8a53321fe184debff477e9e41e67e3134703e2` — `validateRequestedConfig()` explicitly accepts only `kNone` and `kGnss`; any other value is rejected before cross-field validation.
- `9b26a6ab10a559f12d23f712c3a7da082c322753` — regression tests cover undefined source values with tracking both disabled and enabled and require both effective services to fail closed as `BLOCKED / INVALID_CONFIGURATION`.

Current legacy TRACKER/RELAY/BASE projections only produce valid `NONE/GNSS` values, so this was not reachable through the current physical runtime source. It is nevertheless a required boundary fix before a future mutable parser/configuration owner is introduced.

## P3 finding — relay-apply interleaving coverage was limited

Astra found no concrete concurrency defect in `RadioManager::setRelayForwardingEnabled()`, but noted that the new setter path did not directly freeze several R2 ownership/interleaving cases even though analogous role-transition behavior was already tested.

### Test closure

`6d6f2dc756263020bda6596a10459b7d708d22f3` extends the existing R2 fake-radio harness without changing production radio code. It now directly verifies:

- gate contention defers the apply without any driver entry;
- already handed-off RX is drained under the old forwarding behavior;
- pending old hardware work is dispatched/drained before the behavior epoch advances;
- a delayed old GPIO/semaphore wake cannot relabel retained payload under the new epoch;
- a pending legacy role transition defers the explicit relay apply;
- RX is restored after a successful behavior transition;
- TRACKER + relay forwarding enabled still sends its own POSITION traffic;
- the previously covered active-TX disable path still cannot abort or reinterpret in-flight relay TX.

No production RadioManager/NetworkService state machine was rewritten for this P3 closure.

## Final owner validation after Astra fixes

Owner-run `./firmware/tests/run_host_tests.sh`: **PASS**.

The suite included B1A/B2/B3/B4, M3/R3/M4/M5, R2/R2.1, the expanded B4 RadioManager tests, all five production startup scenarios, and R4 Wire/I2C/power/watchdog checks under the existing warning/sanitizer policy.

Owner-run `pio run -e rak4630`: **SUCCESS** using Nordic nRF52 platform 11.0.0 and GCC ARM 7.2.1. The build verified/applied the pinned R4 Wire and R2.1 SX126x driver-gate patches.

Final measured image:

- RAM: `13,852 / 248,832` bytes = **5.6%**;
- Flash: `140,184 / 815,104` bytes = **17.2%**.

No new project warning was present in the supplied build output.

## Compatibility and physical-evidence boundary

The Astra fixes do not change:

- TLP v1 packet bytes or sizes;
- golden compatibility fixtures;
- RF PHY/config or existing packet airtime;
- relay one-hop/nested policy, queue size, dedupe key or deterministic delay;
- device identity or sequence semantics;
- history/journal layout or store-before-send ordering;
- GNSS acquisition/freshness or GNSS power ownership;
- USB ROLE syntax or AUTO heuristic;
- remote/security surface.

Existing owner-operated physical evidence remains limited to the tested direct path:

- Tracker B `0E8ADE7E71531AA3`, B4 composition image;
- legacy Base A `09A462BD4B275BA5` unchanged;
- tracker runtime `ROLE TRACKER mode=AUTO`;
- Base observation: `BASE RX NEW source=0E8ADE7E71531AA3 seq=4096 path=DIRECT rssi=-71 snr=9`.

That supports fresh GNSS -> store-before-send -> frozen TLP v1 POSITION -> direct RF -> old Base reception for the tested image. It does not prove flash power-cut recovery, long-range RF, current consumption, multi-hop, BASE+relay coexistence or independently configured TRACKER+relay hardware behavior.

The Astra P2 fix is unreachable from today's frozen legacy request projections, and the P3 closure changes tests only. Therefore no repeat physical GNSS/RF run is required solely for these audit fixes. This must be revisited when a mutable configuration ingress is introduced.

## Final verdict

**B4 is merge-ready at post-audit code/test head `6d6f2dc756263020bda6596a10459b7d708d22f3`, subject to the normal rule that any further source change invalidates this closure until affected validation is repeated.**
