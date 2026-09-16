# PRE-M6 Stack Audit Resolution

Status: **ASTRA P2 FINDINGS FIXED IN SOFTWARE; OWNER HOST/RAK BUILD PASS; INDEPENDENT RE-AUDIT + PHYSICAL M6A GATE PENDING**.

Audit baseline: `main@859ca4af0abf9f533a54227b38d2b1a5ddcfcccb`.
Audited candidate: `docs/m6-premerge-sync@8273e2434d0339a1335a9f1f4e4488825819c7bf`.
Fix branch: `fix/m6-audit-findings`.
Code-fix head before this resolution note: `5eb63ac1b92560dd7435cc975eefac83bb7cd290`.

## Independent Astra result

The independent audit found no P0/P1 issue, but identified four P2 findings that had to be corrected before the physical M6A gate or merge:

1. `M6A-01` — LIS3DH high-resolution output could be accepted before the documented `7/ODR` turn-on interval had elapsed.
2. `M6A-02` — after the immediate three-attempt shutdown budget was exhausted, the manager entered a terminal state and could leave an always-powered LIS3DH sampling at 10 Hz even if the I2C bus later recovered.
3. `M6A-03` — retained nonzero `ACT_THS` was not cleared, so autonomous activity/inactivity mode from an earlier MCU/image session could survive into the M6A probe configuration.
4. `M6C-01` — the planar field-geometry domain admitted exact pole/longitude-seam coordinates (`latitude = +/-90`, `longitude = +/-180`) where equivalent physical points can have multiple coordinate representations and therefore inconsistent planar classification.

The audit independently reran the full host suite and RAK4630 build and reproduced the findings with adversarial probes. It explicitly kept physical RAK1904 behavior as **NOT PROVEN**.

## Fixes

### M6A-01 — explicit high-resolution settling

- `CTRL_REG1=0x27` remains the final configuration write.
- A rollover-safe `kHighResolutionSettleMs = 7 * kProbeSamplePeriodMs` gate now starts from the successful ODR-enable write.
- No DRDY/status or axis read is allowed before this gate expires.
- The retained first ready XYZ set is still discarded after settling.
- A later ODR period is then required before the accepted probe sample.
- The normal sample timeout budget starts only after settling, instead of competing with the mandatory HR turn-on time.

This fixes sample-quality timing without changing protocol, RF, storage, role, GNSS or power-rail ownership.

### M6A-02 — sparse post-fault cleanup ownership

- The existing immediate shutdown budget remains three cooperative `CTRL_REG1=0` attempts.
- If all three fail, capability state becomes PRESENT + FAULT immediately and the captured sample remains suppressed.
- The manager no longer abandons a positively identified sensor in an unconfirmed active state.
- A fault-cleanup state retries one shutdown write only after a long cooperative backoff (`60 s`).
- If the bus later recovers, the sensor is powered down; capability remains faulted until reboot/re-probe rather than silently returning to healthy state.
- A permanently failed bus therefore causes sparse bounded work, not a tight retry loop.

This is an internal LIS3DH cleanup policy only. It does not touch GNSS `WB_IO2/3V3_S` ownership.

### M6A-03 — retained autonomous-mode reset

- `ACT_THS (0x3E)` is explicitly written to zero while the device is powered down.
- `CTRL_REG1=0` remains first and 10 Hz enable remains last.
- The configuration write count is kept bounded; FIFO enable is already explicitly cleared through `CTRL_REG5`, so a redundant `FIFO_CTRL` reset was removed rather than increasing the number of cooperative passes.

### M6C-01 — fail closed at planar singularities

M6C remains deliberately local/planar. Rather than add speculative spherical or antimeridian geometry:

- exact `latitude = +90` and `latitude = -90` are outside the accepted M6C geometry domain;
- exact `longitude = +180` and `longitude = -180` are outside the accepted M6C geometry domain;
- coordinates immediately inside those limits remain valid subject to the existing 10-degree span bound;
- invalid query coordinates return `kInvalidPoint`, not `kOutside`.

This makes the unsupported global domain explicit and fail-closed.

## Regression coverage added/strengthened

M6A tests now freeze:

- no axis read before the 7/ODR HR-settle deadline even if DRDY is already asserted;
- retained `ACT_THS` is cleared;
- retained first sample is discarded only after settling;
- accepted sample comes from a later ODR phase;
- HR-settle timing is rollover-safe;
- three shutdown failures publish FAULT and suppress the sample;
- no extra cleanup bus operation occurs before the sparse retry deadline;
- later bus recovery performs a successful shutdown while capability remains faulted.

M6C tests now freeze:

- exact +/-180-degree longitude rejection;
- exact +/-90-degree latitude rejection;
- singular query coordinates return invalid rather than outside;
- near-limit local polygons immediately inside the accepted coordinate domain remain valid.

## Owner-run post-fix validation

On `fix/m6-audit-findings` after all four fixes:

- complete `./firmware/tests/run_host_tests.sh`: **PASS**;
- `M6A bounded RAK1904 detection/sample checks: PASS`;
- `M6C bounded geofence polygon geometry checks: PASS`;
- `M6C unsupported global-domain rejection checks: PASS`;
- `M6C permitted geofence area-set checks: PASS`;
- all retained B1A/B2/B3/B4, M3/M4/M5, R2/R3/R4 and startup regressions: **PASS**;
- `pio run -e rak4630`: **SUCCESS** with GCC ARM 7.2.1;
- R4 bounded Wire transform: verified/applied;
- R2.1 SX126x driver-gate transform: verified/applied;
- RAM: **13,932 / 248,832 bytes (5.6%)**;
- flash: **142,184 / 815,104 bytes (17.4%)**;
- no new ORUN compiler warning was shown; only the known pinned SX126x third-party warnings remain.

Compared with the previously audited M6C2 image (`141,928` flash), the runtime audit fixes add **256 bytes flash** and no measured RAM increase.

## Compatibility/system impact

```text
TLP v1 bytes/sizes:            unchanged
POSITION/RELAY_FORWARD codec: unchanged
RF PHY/airtime policy:        unchanged
M5 forwarding/dedupe:         unchanged
History journal/flash region: unchanged
Store-before-send:            unchanged
Identity/sequence:            unchanged
GNSS state machine:           unchanged
GNSS WB_IO2/3V3_S ownership: unchanged
Legacy ROLE/AUTO behavior:    unchanged
Durable configuration:        not added
Security/contact semantics:   unchanged
```

M6B1/M6B2 remain software-only and unchanged by these fixes. M6C1 domain acceptance is narrower only at exact global coordinate singularities; M6C2 inherits that fail-closed geometry contract.

## Remaining gates

1. **Independent Astra re-audit** of the four minimal fixes and their new tests.
2. If the re-audit finds no physical-path blocker, upload the latest corrected image to Tracker B when hardware is available.
3. Perform focused physical M6A validation: positive WHO_AM_I path, real settled/fresh XYZ, and successful post-sample shutdown behavior.
4. Record the physical evidence without promoting it to continuous sampling, current-consumption, activity-classification or geofence-field validation.
5. Merge only after both re-audit closure and the physical M6A gate are complete.

No physical PASS is claimed by this document.
