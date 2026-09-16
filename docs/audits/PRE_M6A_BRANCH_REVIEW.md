# PRE-M6A Branch Review

Status: **POST-ASTRA SOFTWARE REVALIDATED; INDEPENDENT RE-AUDIT + PHYSICAL CHECK PENDING**.

Baseline: `main@859ca4af0abf9f533a54227b38d2b1a5ddcfcccb`.
Original branch: `feat/m6a-accelerometer-foundation`.
Current correction branch: `fix/m6-audit-findings`.
Stack audit resolution: `docs/audits/PRE_M6_STACK_AUDIT_RESOLUTION.md`.

## Scope

This review covers the M6A RAK1904/LIS3DH foundation only: capability presence/health projection, bounded I2C detection/configuration/sample behavior, cooperative-loop ownership, low-power cleanup and regression coverage. It does not validate cattle activity classification, geofence runtime behavior, new RF payloads or physical RAK1904 operation.

## Invariants checked

- TLP v1 bytes and packet sizes remain unchanged.
- RF parameters, M5 one-hop forwarding and relay ownership remain unchanged.
- GNSS acquisition, freshness and 3V3_S/WB_IO2 ownership remain unchanged.
- RAK1904 presence does not imply role/profile/service enablement.
- PRESENT + FAULT remains distinct from ABSENT.
- I2C work remains bounded by the existing R4 Wire/recovery path.
- M6A does not allocate flash or add persistence ownership.
- No raw XYZ data is transmitted.

## Earlier finding M6A-R1 — retained LIS3DH output could be reported as fresh

Severity: **P2 / correctness**.

RAK1904 is supplied from WisBlock VDD, so LIS3DH can remain powered across MCU reset/DFU. The original sequence could relabel retained output as fresh.

Fix retained in current code:

- `CTRL_REG1=0` first;
- configure while powered down;
- enable 10 Hz last;
- discard the first ready XYZ set;
- accept only a later newly generated sample.

## Earlier finding M6A-R2 — one failed shutdown could leave sensor running

Severity: **P2 / power + recovery**.

The original cleanup tried `CTRL_REG1=0` once. This was first hardened to three immediate cooperative attempts.

The later independent Astra audit found that terminating after those three failures still abandoned cleanup ownership. That superseding issue is `M6A-02` below.

## Independent Astra finding M6A-01 — high-resolution settle time missing

Severity: **P2 / correctness**.

The pre-fix flow accepted a later ODR sample long before the LIS3DH documented high-resolution `7/ODR` turn-on interval had elapsed. At 10 Hz this is 700 ms.

Current fix:

- `kHighResolutionSettleMs = 7 * kProbeSamplePeriodMs`;
- settle timing begins only after successful final `CTRL_REG1=0x27` write;
- no DRDY/status/axis read before the rollover-safe settle deadline;
- retained first ready XYZ set is still discarded after settling;
- one later ODR period is required before the accepted sample;
- sample timeout budget starts after mandatory settling.

## Independent Astra finding M6A-02 — cleanup abandoned after immediate retry budget

Severity: **P2 / power + recovery**.

Three failed shutdown writes previously produced PRESENT + FAULT but entered a terminal state. If the shared bus later recovered, the always-powered LIS3DH could remain at 10 Hz until reboot.

Current fix:

- three immediate shutdown attempts remain bounded and cooperative;
- after exhaustion, capability becomes PRESENT + FAULT and the captured sample is suppressed;
- manager retains a sparse fault-cleanup state;
- one `CTRL_REG1=0` write is retried after a 60-second backoff;
- a permanently bad bus therefore causes sparse bounded work rather than a tight loop;
- successful later cleanup powers the sensor down but does not silently change health back to OK; reboot/re-probe is required.

## Independent Astra finding M6A-03 — retained ACT_THS not cleared

Severity: **P2 / retained sensor-state correctness**.

A nonzero `ACT_THS` retained across MCU/image changes can enable LIS3DH autonomous activity/inactivity mode and interfere with the intended HR probe state.

Current fix:

- `ACT_THS (0x3E)` is explicitly cleared while powered down;
- `CTRL_REG1=0` remains the first configuration write;
- 10 Hz enable remains the final write;
- redundant FIFO control reset was removed because FIFO enable is already explicitly cleared by `CTRL_REG5`, preserving the bounded configuration-pass count.

## Current owner-run revalidation evidence

After all Astra fixes on `fix/m6-audit-findings`:

- complete `./firmware/tests/run_host_tests.sh`: **PASS**;
- M6A bounded RAK1904 detection/sample checks: **PASS**;
- HR-settle-before-read regression: **PASS**;
- retained `ACT_THS` cleanup regression: **PASS**;
- three-failure -> FAULT -> later sparse shutdown recovery regression: **PASS**;
- HR settling rollover regression: **PASS**;
- all retained B1A/B2/B3/B4, M3/M4/M5, R2/R3/R4 and startup regressions: **PASS**;
- `pio run -e rak4630`: **SUCCESS** with GCC ARM 7.2.1;
- R4 bounded Wire transform verified/applied;
- R2.1 SX126x driver-gate transform verified/applied;
- RAM: **13,932 / 248,832 bytes (5.6%)**;
- flash: **142,184 / 815,104 bytes (17.4%)**;
- only the already-known pinned SX126x third-party warnings were shown.

Compared with the initial Astra-audited image (`141,928` flash), the four stack fixes add **256 bytes flash** and no measured RAM increase.

## Remaining closure gates

1. independent Astra re-audit of the four minimal fixes and tests;
2. focused physical RAK1904 validation on the latest corrected image only after re-audit: positive WHO_AM_I path, settled/fresh XYZ and successful post-sample shutdown behavior.

The previously programmed Tracker B image predates these final corrections. Host/build success does **not** make RAK1904 physically validated, and no cattle activity-accuracy or current-consumption claim is made here.
