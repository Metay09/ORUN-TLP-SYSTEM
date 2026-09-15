# PRE-M6A Branch Review

Status: **FIXES APPLIED; REVALIDATION PENDING**.

Baseline: `main@859ca4af0abf9f533a54227b38d2b1a5ddcfcccb`.
Branch: `feat/m6a-accelerometer-foundation`.

## Scope

This review covers the M6A RAK1904/LIS3DH foundation only: capability presence/health projection, bounded I2C detection/configuration/sample behavior, cooperative-loop ownership, low-power cleanup and regression coverage. It does not validate cattle activity classification, geofence behavior, new RF payloads or physical RAK1904 operation.

## Invariants checked

- TLP v1 bytes and packet sizes remain unchanged.
- RF parameters, M5 one-hop forwarding and relay ownership remain unchanged.
- GNSS acquisition, freshness and 3V3_S/WB_IO2 ownership remain unchanged.
- RAK1904 presence does not imply role/profile/service enablement.
- PRESENT + FAULT remains distinct from ABSENT.
- I2C work remains bounded by the existing R4 Wire/recovery path.
- M6A does not allocate flash or add persistence ownership.
- No raw XYZ data is transmitted.

## Finding M6A-R1 — retained LIS3DH output could be reported as fresh

Severity: **P2 / correctness**.

RAK1904 is supplied from WisBlock VDD, so the LIS3DH can remain powered across an MCU reset/DFU. LIS3DH power-down preserves configuration and the most recent output registers. The earlier M6A sequence did not first force `CTRL_REG1=0`, and it accepted the first data-ready output after enabling 10 Hz. A retained pre-reset sample could therefore be exposed with a new `captured_at_ms` and incorrectly look fresh.

Fix:

- force `CTRL_REG1=0` as the first cooperative configuration write;
- configure remaining registers while powered down;
- start 10 Hz by writing `CTRL_REG1=0x27` last;
- read and discard the first data-ready XYZ set;
- wait one further ODR period before accepting the probe sample;
- regression test uses different retained and fresh values and asserts only the fresh set is published.

## Finding M6A-R2 — one failed shutdown could leave the sensor running

Severity: **P2 / power + recovery**.

The earlier cleanup attempted `CTRL_REG1=0` once. A transient NACK/recovered bus timeout at that exact point would mark the capability faulted but could leave the always-powered LIS3DH sampling at 10 Hz until reboot, which is the wrong fail-safe direction for a battery tracker.

Fix:

- sensor shutdown remains one I2C operation per cooperative loop pass;
- shutdown is retried up to `kPowerDownMaxAttempts = 3`;
- a transient first failure can recover and still publish PRESENT only after a confirmed shutdown;
- persistent failure ends as PRESENT + FAULT and never exposes the captured sample as consumable;
- diagnostics count each failed shutdown attempt.

## Validation status

Before these audit fixes, the owner had already demonstrated full host PASS, RAK4630 build SUCCESS and DFU programming SUCCESS for the preceding M6A image. Those results do **not** validate the new R1/R2 fixes.

Required next validation for the current branch:

1. complete host regression suite;
2. RAK4630 production build and RAM/flash comparison;
3. focused physical RAK1904 `WHO_AM_I` + real XYZ + confirmed power-down observation when the operator is next at the hardware;
4. independent final audit before merge.

No physical RAK1904 PASS is claimed by this review.
