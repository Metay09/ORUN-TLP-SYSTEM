# PRE-M6 Stack Audit Package

Status: **ASTRA INITIAL AUDIT COMPLETE; P2 FIXES APPLIED; INDEPENDENT RE-AUDIT + PHYSICAL M6A GATE PENDING**.

Audit baseline: `main@859ca4af0abf9f533a54227b38d2b1a5ddcfcccb`.
Initially audited candidate: `docs/m6-premerge-sync@8273e2434d0339a1335a9f1f4e4488825819c7bf`.
Current fix branch: `fix/m6-audit-findings`.
Detailed finding/fix record: `docs/audits/PRE_M6_STACK_AUDIT_RESOLUTION.md`.

## Purpose

This package defines the exact scope and evidence an independent reviewer should
assess before the current M6 stack is merged. It is intentionally explicit about
what is runtime-integrated, what is host-only, and what remains physically
unverified.

The reviewer should treat repository code/tests/golden fixtures and the current
architecture rules as canonical. Historical discussions must not override the
repository.

## Stack checkpoints

```text
main baseline
  859ca4af0abf9f533a54227b38d2b1a5ddcfcccb

M6A accelerometer foundation
  a4e5a9b1ae7f1143c4ec70441a51e0b1c978329d

M6B1 activity window/features closure
  8353e18785cd16d62c43f0c0d5f63418c96a3d0f

M6B2 activity quality closure
  5400977c971cf118c65a134515fc75d4b8dec00e

M6C1 geofence geometry closure
  853622c4849658ec215ae698364c577c1133614b

M6C2 permitted-area union closure
  4858db8e19318ba7cf007fd94d2765b3f9084c0b

initial Astra-audited docs/code candidate
  8273e2434d0339a1335a9f1f4e4488825819c7bf

post-audit fix code head
  5eb63ac1b92560dd7435cc975eefac83bb7cd290
```

## Initial Astra audit result

The independent audit found no P0/P1 issue. Four P2 findings were reproduced:

- `M6A-01`: high-resolution sample acceptance before LIS3DH `7/ODR` settling;
- `M6A-02`: terminal shutdown-fault path could leave an always-powered sensor at
  10 Hz after later bus recovery;
- `M6A-03`: retained nonzero `ACT_THS` was not cleared;
- `M6C-01`: exact pole/longitude-seam coordinate singularities were accepted by
  the local planar geometry model.

All four have been addressed on `fix/m6-audit-findings`. See
`PRE_M6_STACK_AUDIT_RESOLUTION.md` for the exact fixes, tests and owner-run
post-fix evidence. These fixes must be independently re-audited before the
physical M6A gate is attempted.

## Runtime versus host-only boundary

### Runtime-integrated

Only M6A changes production composition:

- `AccelerometerManager` is started from `main.cpp`;
- RAK1904/LIS3DH bounded detection is part of boot/runtime polling;
- accelerometer support/presence/health is projected into `CapabilitySnapshot`;
- one bounded probe sample may be printed to USB diagnostics;
- sensor shutdown is part of the bounded probe path;
- after immediate shutdown failures, sparse fault cleanup retains ownership until
  a later cooperative shutdown succeeds.

M6A must not alter role/profile ownership, GNSS power ownership, tracking
semantics, relay forwarding, TLP bytes, storage format or RF behavior.

### Host-only / not runtime-integrated

The following compile in the production source tree but are not referenced by
`main.cpp`:

- `activity_window.*`;
- `activity_quality.*`;
- `geofence_geometry.*`;
- `geofence_area_set.*`.

They must therefore be reviewed as portable deterministic primitives, not as
proof of production activity/geofence behavior.

## M6A re-audit focus

The independent reviewer should specifically verify the post-audit changes:

1. `CTRL_REG1=0x27` remains the final enable write;
2. no DRDY/status/axis read occurs before the rollover-safe `7/ODR` HR settling
   deadline;
3. the retained first ready XYZ set is still discarded after settling and a later
   ODR sample is the only probe sample exposed;
4. `ACT_THS` is explicitly cleared while powered down;
5. normal sample timeout starts after mandatory HR settling rather than expiring
   during the settle window;
6. three immediate shutdown failures publish PRESENT + FAULT and suppress the
   captured sample;
7. faulted cleanup retries are sparse/cooperative and can power down the sensor if
   the bus later recovers without silently changing capability health back to OK;
8. a permanently bad bus does not create a tight retry loop;
9. UNKNOWN/PRESENT/ABSENT semantics still do not conflate transport failure with
   hardware absence;
10. RAK1904 VDD ownership still does not touch GNSS `WB_IO2/3V3_S`;
11. R4 bounded Wire/recovery and watchdog assumptions remain intact;
12. capability presence does not imply activity service enablement.

## M6B review focus

M6B1/M6B2 intentionally stop before animal classification.

Review that:

- the 50-sample fixed-memory feature window has no dynamic allocation/raw-window
  buffer;
- accumulator widths remain safe over the full `int16_t` sample domain;
- timing continuity is rollover-safe and treats intervals outside 50..300 ms as
  unreliable without silently dropping samples;
- eligibility fails closed for incomplete, inconsistent or impossible-duration
  feature objects;
- activity magnitude/variance/delta values do not leak into the eligibility gate;
- no RESTING/GRAZING/WALKING threshold or animal-accuracy claim is frozen from
  synthetic tests;
- no RF/storage/power/runtime behavior is introduced by M6B1/M6B2.

## M6C re-audit focus

Review that:

- polygon validation is deterministic and fail-closed;
- cross-product and translated shoelace arithmetic stay within signed-64-bit
  bounds for the documented 10-degree/64-effective-vertex domain;
- exact `latitude = +/-90` and `longitude = +/-180` are rejected as unsupported
  local-planar singularities rather than approximated;
- coordinates immediately inside those singular limits remain accepted subject to
  the existing span and polygon-validity rules;
- invalid singular query coordinates return `kInvalidPoint`, not `kOutside`;
- explicit closing vertices preserve the 64-effective-vertex contract;
- self-intersection/degenerate/invalid-coordinate cases are rejected;
- INSIDE/BOUNDARY/OUTSIDE geometry works for concave and both winding directions;
- area-set composition validates the complete configured set before accepting an
  early containment result;
- INSIDE outranks BOUNDARY independent of polygon order;
- no arbitrary durable/product area-count limit is silently frozen;
- geometry does not own GNSS freshness/source arbitration;
- no NEAR_FENCE, hysteresis, repeated-fix, FREE_GRAZE or LOST behavior is claimed.

## Compatibility and architecture invariants

The review must confirm the stack does not change:

```text
TLP v1 packet bytes/sizes
POSITION / RELAY_FORWARD encoding
M5 one-hop + nested-relay policy
RF frequency / SF / bandwidth / coding rate / TX power
history journal format or 0xED000..0xF4000 ownership
store-before-send semantics
device identity or sequence semantics
GNSS acquisition/freshness state machine
GNSS 3V3_S/WB_IO2 ownership
legacy USB ROLE syntax / AUTO bootstrap behavior
```

Also verify these separations remain true:

```text
Role != Location Source != GNSS Power != Capability
     != Transport != Identity != Profile != User Identity

accelerometer present != activity enabled
GNSS present != geofence enabled
TX_DONE != network contact
OUTSIDE != LOST
```

## Owner-run post-fix validation evidence

On `fix/m6-audit-findings` after the four P2 fixes:

- complete `./firmware/tests/run_host_tests.sh`: **PASS**;
- host profiles include `-Wall -Wextra -Werror` and ASan/UBSan;
- `M6A bounded RAK1904 detection/sample checks: PASS`;
- `M6C bounded geofence polygon geometry checks: PASS`;
- `M6C unsupported global-domain rejection checks: PASS`;
- `M6C permitted geofence area-set checks: PASS`;
- all retained B1A/B2/B3/B4, M3/M4/M5, R2/R3/R4 and startup regressions:
  **PASS**;
- `pio run -e rak4630`: **SUCCESS** with GCC ARM 7.2.1;
- R4 bounded Wire transform verified/applied;
- R2.1 SX126x driver-gate transform verified/applied;
- current linked image: **13,932 / 248,832 bytes RAM (5.6%)**;
- current linked image: **142,184 / 815,104 bytes flash (17.4%)**;
- no new ORUN compiler warning observed; known warnings remain inside pinned
  third-party SX126x sources.

This is software evidence only. It does not close the physical RAK1904 gate.

## Physical evidence and explicit non-evidence

Historical evidence exists for an earlier pre-audit cooperative M6A image being
programmed to Tracker B. That does **not** close current M6A because the physical
image predates the later audit findings and fixes.

Current physical status:

```text
latest corrected M6A upload:                     PENDING
RAK1904 positive WHO_AM_I path on corrected image:PENDING
settled/fresh XYZ on corrected image:             PENDING
post-sample shutdown on corrected image:          PENDING
sparse fault-cleanup recovery on hardware:        NOT PROVEN
continuous activity sampling:                     NOT IMPLEMENTED
current-consumption measurement:                  NOT PERFORMED
animal behavior accuracy:                         NOT VALIDATED
geofence field behavior:                          NOT RUNTIME-INTEGRATED
trusted LOST/contact:                             NOT IMPLEMENTED
```

The independent reviewer must not promote host/build evidence into any of those
physical/product claims.

## Required re-audit output

Report findings by severity and exact file/symbol. Distinguish:

- unresolved correctness/safety defects that block physical test/merge;
- regressions introduced by the four fixes;
- test coverage gaps;
- architecture/ownership contradictions;
- documentation inaccuracies;
- future-only recommendations that should **not** expand this milestone.

For every real finding, state whether it affects:

```text
wire compatibility
mixed-fleet behavior
RF airtime/capacity
RAM/flash
power/sleep
I2C/concurrency/ownership
persistence/power-cut integrity
security
physical validation scope
```

Do not propose speculative generic HALs, registries, event buses, new protocol
families or unrelated future features merely to make the code more abstract.

## Merge gates after re-audit

The current stack is not merge-ready until both are true:

1. independent Astra re-audit confirms the P2 fixes or any new real findings are
   corrected and revalidated;
2. focused physical M6A validation is completed on the latest corrected image.

If the re-audit finds a code issue affecting the physical M6A path, fix it first
and run the physical check only on the corrected image.
