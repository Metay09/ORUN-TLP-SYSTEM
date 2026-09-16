# PRE-M6 Stack Audit Resolution

Status: **ASTRA P2 FINDINGS CLOSED; FINAL INDEPENDENT SOFTWARE AUDIT PASS; FOCUSED OPERATOR M6A PHYSICAL GATE PASS / CLOSED**.

Audit baseline: `main@859ca4af0abf9f533a54227b38d2b1a5ddcfcccb`.
Initially audited candidate: `docs/m6-premerge-sync@8273e2434d0339a1335a9f1f4e4488825819c7bf`.
Corrected/re-audited candidate: `fix/m6-audit-findings@613cdf1ab583d4957e15ac0a90c6785cfbff641b`.

Final audited diagnostic candidate: `fix/m6-audit-findings@332cf0e1b307735348a97c3cbd15f916d04a21a0`.

## Independent Astra result

The initial independent audit found no P0/P1 issue, but identified four P2 findings that had to be corrected before the physical M6A gate or merge:

1. `M6A-01` — LIS3DH high-resolution output could be accepted before the documented `7/ODR` turn-on interval had elapsed.
2. `M6A-02` — after the immediate three-attempt shutdown budget was exhausted, the manager entered a terminal state and could leave an always-powered LIS3DH sampling at 10 Hz even if the I2C bus later recovered.
3. `M6A-03` — retained nonzero `ACT_THS` was not cleared, so autonomous activity/inactivity mode from an earlier MCU/image session could survive into the M6A probe configuration.
4. `M6C-01` — the planar field-geometry domain admitted exact pole/longitude-seam coordinates (`latitude = +/-90`, `longitude = +/-180`) where equivalent physical points can have multiple coordinate representations and therefore inconsistent planar classification.

The initial audit independently reran the full host suite and RAK4630 build and reproduced the findings with adversarial probes. It explicitly kept physical RAK1904 behavior as **NOT PROVEN**.

## Fixes

### M6A-01 — explicit high-resolution settling

- `CTRL_REG1=0x27` remains the final configuration write.
- A rollover-safe `kHighResolutionSettleMs = 7 * kProbeSamplePeriodMs` gate starts from the successful ODR-enable write.
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
- FIFO use remains disabled through explicit `CTRL_REG5.FIFO_EN=0`; removing a redundant `FIFO_CTRL` reset does not enable retained FIFO state.

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
- near-limit local polygons immediately inside the accepted coordinate domain remain valid;
- M6C2 propagates the stricter invalid-domain result across the full permitted-area set.

## Owner-run post-fix validation

Historical evidence at `613cdf1`, after all four fixes and before `ACCEL?`:

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

## Independent Astra re-audit closure

Astra independently re-audited `8273e2434d0339a1335a9f1f4e4488825819c7bf..613cdf1ab583d4957e15ac0a90c6785cfbff641b` and reported:

```text
M6A-01 -> CLOSED
M6A-02 -> CLOSED
M6A-03 -> CLOSED
M6C-01 -> CLOSED
new P0/P1/P2 blocker -> none found
```

The re-audit independently reran:

- full host suite: **PASS**;
- `pio run -e rak4630`: **SUCCESS**;
- RAM/flash: **13,932 B / 142,184 B**;
- additional ASan/UBSan adversarial probes: **PASS**.

The independent re-audit specifically confirmed:

- 700 ms HR settling starts after final ODR enable and does not consume the later 500 ms sample timeout;
- DRDY/output is not inspected during settling;
- the retained first XYZ set remains discarded and a later ODR phase is required for the accepted sample;
- production `main.cpp` continues to call `AccelerometerManager::poll()` after `detectionComplete=true`, so sparse fault cleanup remains live;
- no cleanup I2C transaction occurs before the 60-second deadline, one shutdown operation occurs at the deadline, repeated failure re-arms another sparse deadline, and later bus recovery powers the sensor down while capability remains FAULT;
- `ACT_THS=0` is applied while powered down and retained FIFO mode cannot become active because `CTRL_REG5.FIFO_EN=0` remains explicit;
- exact pole/seam geometry coordinates fail closed, immediately interior coordinates remain supported, and M6C2 propagates invalid-domain polygons through full-set validation;
- TLP codecs/golden fixtures, NetworkService, RadioManager, PositionFlow, HistoryStore/journal, GNSS state machine, identity/sequence, legacy role mapping and RF PHY/config remain unchanged.

At `613cdf1`, Astra found no code finding blocking the focused physical M6A test.
That re-audit is software evidence only and does **not** constitute physical
RAK1904 validation; the later operator evidence is recorded separately below.

## Final diagnostic-delta Astra audit

Astra independently audited exactly `613cdf1ab583d4957e15ac0a90c6785cfbff641b..332cf0e1b307735348a97c3cbd15f916d04a21a0`.

- new P0: **none found**;
- new P1: **none found**;
- new P2: **none found**;
- full host suite: **PASS**;
- supplemental ASan/UBSan production-code probes: **PASS**;
- clean `pio run -e rak4630`: **SUCCESS**;
- current linked RAM: **13,948 / 248,832 bytes (5.6%)**;
- current linked flash: **142,456 / 815,104 bytes (17.5%)**;
- delta versus `613cdf1`: **+16 bytes RAM, +272 bytes flash**;
- only three known third-party warnings: `USING RAK4630` and two SimpleTimer
  signedness warnings; **no ORUN compiler warnings**.

`ACCEL?` is diagnostic-only and reports the latched boot result without probing,
waking, reconfiguring or re-entering the accelerometer manager. Automatic event
reporting, sample lifetime, serial buffering and legacy ROLE compatibility were
confirmed. No TLP v1, RF, storage, GNSS, identity, sequence, role compatibility or
power-ownership behavior change was found. The delta is clear for physical-evidence
documentation and merge preparation. These are independent software results,
not independent Astra hardware validation.

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

## Focused operator physical gate — CLOSED

The operator reported the following on Tracker B running the exact current
image `332cf0e1b307735348a97c3cbd15f916d04a21a0`. This is **operator physical
evidence**, not independently reproduced Astra hardware validation.

1. Before RAK1904 was installed:

   ```text
   ACCEL ABSENT
   ```

2. With RAK1904 installed in SENSOR C:

   ```text
   ACCEL PRESENT x_mg=-182 y_mg=189 z_mg=-916
   ```

3. On a separate hardware reset with the monitor running and **without sending
   `ACCEL?`**:

   ```text
   ACCEL PRESENT x_mg=-186 y_mg=226 z_mg=-914
   GNSS: detected
   GNSS ACQUIRE start
   ROLE TRACKER source=AUTO
   ```

The third observation proves automatic PRESENT emission does not depend on
`ACCEL?`. `ACCEL PRESENT` supports the normal shutdown-write path because the
current manager emits `kPresent` only after successful post-sample
`powerDownSensor` completion. It is **not a current-consumption measurement** and
**does not prove fault-cleanup recovery on hardware**.

| Focused check / limitation | Status |
| --- | --- |
| Current corrected image upload to Tracker B | **PASS — operator evidence** |
| RAK1904 positive physical identity path | **PASS — operator evidence** |
| Normal settled/fresh XYZ probe path | **PASS — operator evidence** |
| Normal post-sample shutdown-write path | **PASS — PRESENT emission after successful shutdown write** |
| ABSENT path with module not installed | **PASS — operator evidence** |
| Automatic PRESENT without `ACCEL?` | **PASS — separate reset observation** |
| Independent Astra hardware validation | **NOT PERFORMED** |
| Physical I2C fault-cleanup/recovery | **NOT PROVEN** |
| Current-consumption measurement | **NOT PERFORMED** |
| Continuous production activity sampling | **NOT IMPLEMENTED** |
| Cattle behavior classification/accuracy | **NOT IMPLEMENTED / NOT VALIDATED** |
| Geofence field behavior | **NOT RUNTIME-INTEGRATED / NOT VALIDATED** |
| Trusted LOST/contact semantics | **NOT IMPLEMENTED** |

The narrow focused M6A physical gate is **PASS / CLOSED**. This does not complete
overall M6 or promote M6B/M6C/M6D to production runtime.

## Documentation and pre-merge preparation

The final independent software audit and focused operator M6A physical gate are
closed for `332cf0e1b307735348a97c3cbd15f916d04a21a0`. Remaining work is review of
this documentation synchronization and pre-merge preparation; another M6A
hardware test is not a remaining gate for this unchanged candidate. Overall M6
remains **IN PROGRESS**. No merge is performed or authorized by this record.
