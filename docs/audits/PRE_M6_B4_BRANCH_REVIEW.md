# Pre-M6 B4 Branch Review

Date: 2026-09-15
Branch: `refactor/pre-m6-b4-config-capability-boundary`
Review baseline: `28d254d1f40710fcedd64deab9ce6c216c8d8992`
Code reviewed through: `3186c96d54f0f84243296a776a0f1ffa5ed4c526`
Status: **branch/code/architecture review complete; owner host + RAK build revalidation complete; later independent Astra audit remains**.

## Scope

The complete B4 branch diff was reviewed from the B2/B3 closure baseline through
the current code head. The review covered:

- requested/capability/effective configuration model;
- legacy role compatibility projection;
- NetworkService forwarding ownership;
- RadioManager quiescent forwarding-state apply path;
- production composition in `main.cpp`;
- B4 host tests and startup integration coverage;
- current architecture rules and B4 milestone documentation;
- TLP v1, storage, identity, sequence, GNSS power/state-machine and RF compatibility
  boundaries.

No protocol codec, RF PHY/config, history/journal format, device identity mapping,
sequence format, GNSS acquisition/freshness state machine or USB ROLE syntax is
changed by B4.

## Finding B4-R1 — invalid candidate could partially apply

Severity: **P1 / pre-merge correctness gap**. Current production legacy projections
are valid, so this was not an observed field regression; it was a future config
boundary defect.

The architecture requires whole-candidate validation: an invalid requested
configuration must be rejected atomically rather than partially applying fields
that happen to be individually usable. Before this review, an invalid candidate
such as:

```text
tracking = ON
location_source = NONE
relay_forwarding = ON
```

resolved tracking to `BLOCKED / INVALID_CONFIGURATION` but still resolved relay
forwarding to `ENABLED`. That contradicted the documented `validate(candidate)`
boundary and would have been unsafe to freeze before a real mutable configuration
source exists.

Fixed by:

- `489386ec8efc074191d7ac052de104b351d9996d` — resolver now fail-closes the
  entire invalid candidate with `BLOCKED / INVALID_CONFIGURATION`;
- `3186c96d54f0f84243296a776a0f1ffa5ed4c526` — host test freezes atomic
  rejection and separately proves that a valid relay-only request remains valid.

A future authoritative ConfigManager must still reject an invalid candidate
**before replacing the previous requested state**. The resolver's fail-closed
behavior is a defensive boundary, not a substitute for atomic commit semantics.

## Finding B4-R2 — BASE receive + relay coexistence is not implemented

Severity: **accepted bounded limitation; not a current user-visible bug**.

B4 intentionally does not promote legacy `receive_application_position` into
public configuration. In the current `NetworkService::receive()` ordering, relay
forwarding takes precedence over legacy BASE application consumption. Therefore
an internal `BASE + relay_forwarding_enabled=true` combination is not a dual-use
collector+relay mode: direct POSITION is handled by the forwarding path, and a
RELAY_FORWARD envelope is rejected by the one-hop relay path rather than also
being consumed as BASE application data.

This does not change any legacy role behavior and no user-facing independent
relay toggle exists in B4. Do **not** expose arbitrary BASE/collector + relay
coexistence until collector/gateway/subscriber semantics are explicitly designed
and tested. TRACKER + relay forwarding is the currently representable independent
combination covered by the B4 host seam.

## Finding B4-R3 — capability health projection is intentionally coarse

Severity: **accepted B4 scope limitation**.

The pure model can represent `PRESENT + OK/DEGRADED/FAULT/UNAVAILABLE`, but the
current composition only derives GNSS presence from bounded detection. A detected
GNSS is projected as `PRESENT + OK`; later acquisition timeout/recovery remains
owned by the existing GnssManager state machine and is not yet projected into the
B4 health status.

This is acceptable for the current runtime-only seam because no product health UI
or durable configuration consumes the status yet. Documentation and future code
must not claim that B4 already provides complete runtime GNSS health aggregation.

## Finding B4-R4 — resolved relay intent can temporarily await safe application

Severity: **accepted ownership behavior**.

`EffectiveConfig` resolves the target service state, while RadioManager may defer
installing a forwarding-state change during an active TX or pending legacy role
transition. `main.cpp` recomputes and retries the same requested intent every
cooperative loop, so the request is not lost and in-flight work is not
reinterpreted.

This is correct for B4. A future user-visible service-status API must distinguish
resolved intent from instantaneous transport-applied state if that distinction is
exposed externally.

Legacy `setRole()` still installs the historical forwarding default. That is
required for current compatibility. Before a future independent persisted relay
setting is exposed, the role/default migration must ensure there is no observable
intermediate legacy default that can consume RX under the wrong explicit policy.
B4 does not expose that configuration surface yet.

## Compatibility and system impact

| Area | B4 review result |
| --- | --- |
| TLP v1 bytes / packet sizes | unchanged |
| Mixed-fleet direct POSITION | physically observed PASS |
| RF frequency/SF/BW/CR/TX power | unchanged |
| Airtime per existing packet | unchanged |
| Relay one-hop/nested policy | unchanged |
| Relay queue/dedupe/delay formula | unchanged |
| Device identity | unchanged |
| Sequence semantics | unchanged |
| History/journal layout | unchanged |
| Store-before-send | unchanged |
| GNSS acquisition/freshness state machine | unchanged |
| GNSS power ownership | unchanged |
| USB ROLE syntax/AUTO heuristic | unchanged |
| RAM | 13,852 / 248,832 bytes = 5.6% after B4-R1 revalidation |
| Flash | 140,200 / 815,104 bytes = 17.2% after B4-R1 revalidation |
| Security | no new remote/security surface |

No compatibility fixture or golden packet vector was weakened or changed.

## Physical evidence boundary

Owner-operated B4 regression before the B4-R1 pure resolver fix:

- Tracker B: `0E8ADE7E71531AA3` running B4 composition;
- Base A: `09A462BD4B275BA5` left on the previously validated image;
- Tracker runtime: `ROLE TRACKER mode=AUTO`;
- old Base received:
  `BASE RX NEW source=0E8ADE7E71531AA3 seq=4096 path=DIRECT rssi=-71 snr=9`.

This proves the B4 composition/direct mixed-fleet path through normal fresh GNSS,
store-before-send POSITION generation, frozen TLP v1 and old Base reception for
the tested image.

The later B4-R1 fix changes only behavior for semantically invalid RequestedConfig
candidates. Current production RequestedConfig is derived only from the three
valid frozen legacy projections, so the fix is not reachable in today's hardware
path. A repeat physical GNSS/RF test is therefore not required solely for B4-R1;
host and RAK build revalidation were required and are now complete. This statement
must be revisited when a mutable configuration source can produce/reject real
candidates.

The physical test does not prove flash power-cut recovery, long-range RF, current
consumption, multi-hop, relay coexistence, or independently configured
TRACKER+relay hardware behavior.

## Post-review revalidation

The owner reran validation on branch head
`4cf2828f9338355bcd38c92a62621689b3e46975`, which contains B4-R1, its regression
test and documentation updates.

Full `./firmware/tests/run_host_tests.sh`: **PASS**. The run covered:

- B1A legacy packet golden/malformed checks;
- B2 portable identity and legacy POSITION mapping;
- B3 legacy role compatibility mapping;
- B4 requested/capability/effective config model;
- B4 independent NetworkService relay forwarding seam;
- M3, R3, M4, M5 regressions;
- R2/R2.1 ownership and dependency-patch guards;
- B4 RadioManager independent relay behavior apply;
- B1A RAK A/B identity conversion and serial fixtures;
- production startup identity/history/loop scenarios: mutex, gate, queue, lora,
  success;
- R4 Wire/I2C/power/watchdog guards and nRF watchdog checks.

`pio run -e rak4630`: **SUCCESS** on Nordic nRF52 11.0.0 / GCC ARM 7.2.1.
The build re-applied and verified the pinned R4 Wire and R2.1 SX126x transforms.
Final measured image size:

- RAM: `13,852 / 248,832` bytes = **5.6%**;
- Flash: `140,200 / 815,104` bytes = **17.2%**.

Compared with the pre-review B4 build (`140,168` bytes), B4-R1 adds 32 bytes of
flash and no RAM. No new project warning was shown in the supplied build output.

Because the only post-hardware code change is the invalid-candidate fail-closed
path and all current legacy projections are valid, no repeat physical GNSS/RF run
is required solely for B4-R1.

## Review verdict and remaining gate

**No P0/Critical blocker found.** One pre-merge semantic defect was found and
fixed (B4-R1), and the complete host suite plus RAK4630 build pass after that fix.
The branch remains inside the authorized B4 scope and does not justify a
state-machine rewrite, protocol change, persistent config, BLE, new HAL, new
hardware support or multi-hop work.

Required before B4 closure/merge:

1. update the B4 milestone with this post-review validation result;
2. run the planned independent Astra audit;
3. fix any Astra findings and repeat only the validation affected by those
   findings before merge.
