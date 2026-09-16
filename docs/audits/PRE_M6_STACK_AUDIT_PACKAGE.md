# PRE-M6 Stack Audit Package

Status: **READY FOR INDEPENDENT ASTRA REVIEW; PHYSICAL M6A GATE STILL OPEN**.

Audit baseline: `main@859ca4af0abf9f533a54227b38d2b1a5ddcfcccb`.
Candidate software stack: `feat/m6c2-geofence-area-set@4858db8e19318ba7cf007fd94d2765b3f9084c0b`.
Documentation sync branch: `docs/m6-premerge-sync`.

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
```

## Runtime versus host-only boundary

### Runtime-integrated

Only M6A changes production composition:

- `AccelerometerManager` is started from `main.cpp`;
- RAK1904/LIS3DH bounded detection is part of boot/runtime polling;
- accelerometer support/presence/health is projected into `CapabilitySnapshot`;
- one bounded probe sample may be printed to USB diagnostics;
- sensor shutdown is part of the bounded probe path.

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

## M6A review focus

Detailed software findings and fixes are in
`docs/audits/PRE_M6A_BRANCH_REVIEW.md`.

The independent reviewer should specifically verify:

1. retained LIS3DH output cannot be relabeled fresh after reset/DFU;
2. `CTRL_REG1=0` is established before configuration and 10 Hz enable occurs
   last;
3. the first ready XYZ set is discarded and a later ODR sample is the only probe
   sample exposed;
4. shutdown retries are bounded and cooperative;
5. persistent shutdown failure becomes PRESENT + FAULT and does not expose the
   sample;
6. UNKNOWN/PRESENT/ABSENT and health semantics do not conflate transport failure
   with hardware absence;
7. RAK1904 VDD ownership does not touch GNSS `WB_IO2/3V3_S`;
8. R4 bounded Wire/recovery and watchdog assumptions remain intact;
9. production loop latency/ownership remains cooperative and bounded;
10. capability presence does not imply activity service enablement.

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

## M6C review focus

Review that:

- polygon validation is deterministic and fail-closed;
- cross-product and translated shoelace arithmetic stay within signed-64-bit
  bounds for the documented 10-degree/64-effective-vertex domain;
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

## Owner-run validation evidence

Latest relevant owner-run evidence through M6C2:

- full `./firmware/tests/run_host_tests.sh`: **PASS** after each final slice/fix;
- host profiles include `-Wall -Wextra -Werror` and ASan/UBSan;
- M6 portable B/C helpers are compiled under the repository's `gnu++11` host
  profile matching the old RAK compiler language baseline;
- `pio run -e rak4630`: **SUCCESS** after M6A, M6B1, M6B2, M6C1 and M6C2 closures;
- R4 bounded Wire transform verified/applied;
- R2.1 SX126x driver-gate transform verified/applied;
- current linked image: **13,932 / 248,832 bytes RAM (5.6%)**;
- current linked image: **141,928 / 815,104 bytes flash (17.4%)**;
- no new ORUN compiler warning observed; known warnings remain inside pinned
  third-party SX126x sources.

The unchanged linked image across M6B/M6C is expected because those helpers are
not referenced by production runtime and are removed by the linker.

## Physical evidence and explicit non-evidence

Historical evidence exists for the earlier pre-audit cooperative M6A image being
programmed to Tracker B. That does **not** close current M6A because the installed
image predates M6A-R1/R2.

Current physical status:

```text
latest audit-hardened M6A upload:                 PENDING
RAK1904 positive WHO_AM_I path on latest image:   PENDING
real fresh XYZ sample on latest image:            PENDING
post-sample shutdown call path on latest image:   PENDING
continuous activity sampling:                     NOT IMPLEMENTED
current-consumption measurement:                  NOT PERFORMED
animal behavior accuracy:                         NOT VALIDATED
geofence field behavior:                          NOT RUNTIME-INTEGRATED
trusted LOST/contact:                             NOT IMPLEMENTED
```

The independent reviewer must not promote host/build evidence into any of those
physical/product claims.

## Required audit output

Report findings by severity and exact file/symbol. Distinguish:

- correctness/safety defects that block merge;
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

## Merge gates after audit

The current stack is not merge-ready until both are true:

1. independent Astra audit findings are resolved and relevant tests/builds rerun;
2. focused physical M6A validation is completed on the latest audit-hardened image.

If Astra finds a code issue affecting the physical M6A path, fix it first and run
the physical check only on the corrected image.
