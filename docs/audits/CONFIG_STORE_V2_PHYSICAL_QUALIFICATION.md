# ConfigStore v2 Physical Qualification

Status: **GATE 1 + FRESH-BASELINE POWER-CUT PHYSICAL PASS; ASYNC / NORMAL-SAVE QUALIFICATION OPEN — 2026-09-27**.

Baseline: `main@f6d4f0ca6654f1794fe4625195d6b4e77d2233dd` (PR #46 merged).

Branch: `test/config-store-v2-physical-qualification`.

## Purpose

Close the physical persistence obligations recorded by
`CONFIG_STORE_V2_RUNTIME_CUTOVER_AUDIT_DISPOSITION.md` without adding a
production command surface or requiring SWD/J-Link tooling.

The dedicated PlatformIO target
`rak4630_config_v2_qual_probe` is **test-only**. It links the real:

- `ConfigStore`;
- `NrfConfigFlash`;
- ConfigStore v2 codec/classifier;
- `NrfConfigIncarnationSource`.

SoftDevice remains disabled. The production `rak4630` environment is unchanged.

## Destructive scope

The only destructive command is explicit serial `CLEAN`.

It calls `NrfConfigFlash::erasePage(0)` and `erasePage(1)`, whose hard bounds
are the ConfigStore partition only:

`0x0E9000..0x0EAFFF`.

It must not touch:

- SecurityStore `0x0E7000..0x0E8FFF`;
- BLE bond/InternalFS `0x0EB000..0x0ECFFF`;
- HistoryStore `0x0ED000..0x0F3FFF`.

After both page erases it read-verifies the complete 8 KiB region as `0xFF`
and enters a terminal state requiring a power cycle.

## Read-only STATUS evidence

At boot and on explicit `STATUS`, the probe reports:

- ConfigStore ready / maintenance / token state;
- application committed-override provenance;
- current semantic config;
- baseline commit/failure counters for this boot;
- current 96-bit state token as 64-bit incarnation + 32-bit revision;
- Page A and B classifier evidence;
- generation, token and semantic config for decoded records;
- current-schema reserved-tail erased state.

The status path does not mutate flash.

## Physical closure sequence

### Gate 1 — erased baseline + cold-boot incarnation

1. Upload the test-only probe.
2. Send `CLEAN`; require:
   `CONFIG V2 CLEAN PASS pages=2 all_ff=yes; POWER-CYCLE NOW`.
3. Fully power-cycle.
4. Capture boot status. Expected fresh baseline:
   - store ready=yes;
   - maintenance=no;
   - token_state=VALID;
   - committed_override=no;
   - baseline_commits=1;
   - nonzero incarnation;
   - revision=1;
   - Page A = V2_COMMITTED, generation=1, same incarnation/revision;
   - Page B = ERASED;
   - current-schema tails erased.
5. Fully power-cycle again without CLEAN.
6. Capture boot status. Expected recovery:
   - baseline_commits=0;
   - same exact incarnation;
   - revision remains 1;
   - Page A remains generation 1 committed;
   - Page B remains erased.

Only after the outputs above are captured may this gate be marked physical PASS.

#### Gate 1 physical evidence — PASS

Operator run on RAK4631/RAK4630-class hardware with
`rak4630_config_v2_qual_probe` built from
`test/config-store-v2-physical-qualification@2f9600589a9263460bc58aaf47d75786fe3e05b2`.

Build/upload evidence:

- qualification target build: **SUCCESS**;
- RAM: **9,120 / 248,832 B = 3.7%**;
- Flash: **64,420 / 815,104 B = 7.9%**;
- PlatformIO upload via nrfutil: **SUCCESS**.

Pre-clean physical state was genuine legacy development ConfigStore evidence:

```text
CONFIG V2 BEGIN PASS
CONFIG V2 STORE ready=yes maintenance=yes token_state=UNAVAILABLE
CONFIG V2 TOKEN unavailable
PAGE A = LEGACY_V1_COMMITTED generation=1 interval=180 battery=1
PAGE B = LEGACY_V1_COMMITTED generation=2 interval=180 battery=0
```

This physically confirms the v2 cutover does **not** silently adopt/migrate
legacy v1 state: runtime stays available on safe defaults while semantic
mutation is maintenance-locked.

Explicit `CLEAN` then reported:

```text
CONFIG V2 CLEAN accepted scope=0x0E9000..0x0EAFFF
CONFIG V2 CLEAN PASS pages=2 all_ff=yes; POWER-CYCLE NOW
```

The helper read-verified the complete ConfigStore 8 KiB region as erased before
requesting power-cycle. This is evidence for the helper's ConfigStore-bounded
erase/readback path; it is not an independent readback of adjacent Security,
bond or History partitions.

First full power-cycle from the verified blank partition produced:

```text
CONFIG V2 BEGIN PASS
CONFIG V2 STORE ready=yes maintenance=no token_state=VALID
  committed_override=no tracking_interval_seconds=180 battery_capacity_mah=0
  baseline_commits=1 baseline_failures=0
CONFIG V2 TOKEN incarnation=0xBDC5A737082A143D revision=1
CONFIG V2 PAGE A evidence=V2_COMMITTED tail_erased=yes
  generation=1 incarnation=0xBDC5A737082A143D revision=1
  tracking_interval_seconds=180 battery_capacity_mah=0
CONFIG V2 PAGE B evidence=ERASED tail_erased=yes
```

Second full power-cycle, with no `CLEAN` and no mutation command, produced:

```text
CONFIG V2 BEGIN PASS
CONFIG V2 STORE ready=yes maintenance=no token_state=VALID
  committed_override=no tracking_interval_seconds=180 battery_capacity_mah=0
  baseline_commits=0 baseline_failures=0
CONFIG V2 TOKEN incarnation=0xBDC5A737082A143D revision=1
CONFIG V2 PAGE A evidence=V2_COMMITTED tail_erased=yes
  generation=1 incarnation=0xBDC5A737082A143D revision=1
CONFIG V2 PAGE B evidence=ERASED tail_erased=yes
```

Therefore Gate 1 is **PHYSICAL PASS**:

- blank-partition baseline was physically established;
- incarnation is non-zero;
- baseline token is revision 1 / generation 1;
- no semantic override was fabricated;
- the same exact incarnation survived a true cold power-cycle;
- recovery did not rewrite/re-baseline an already valid v2 partition
  (`baseline_commits=0` on the second boot);
- page evidence and reserved-tail state remained coherent.

No claim is made here for electrical interruption during a write, SoftDevice
async mutation, normal-save cut points or adjacent-partition preservation by
independent dump.

### Gate 2 — fresh-baseline power cut before commit

Test-only target:
`rak4630_config_v2_baseline_powercut_probe`.

Build evidence from
`test/config-store-v2-physical-qualification@b5728b98d92505e56c8f57a4aa8f0fac6b7e5cfd`:

- build: **SUCCESS**;
- RAM: **8,968 / 248,832 B = 3.6%**;
- Flash: **62,896 / 815,104 B = 7.7%**.

The ConfigStore partition was first erased/read-verified by the qualification
helper. The power-cut probe then used the real ConfigStore +
NrfConfigIncarnationSource + NrfConfigFlash path. Its backend wrapper delegated
the real 44-byte body+CRC program and allowed ConfigStore's real stage readback
to complete, then deliberately withheld the exact 4-byte commit program and
reported:

```text
CONFIG V2 BASELINE CUT READY stage=after-body-readback before-commit; CUT POWER NOW
```

The operator removed physical power at that deterministic point.

After physical power was restored with the same image, the production recovery
path reported:

```text
CONFIG V2 CUT RECOVERY begin=PASS ready=yes maintenance=yes
  token_state=UNCERTAIN committed_override=no
  baseline_commits=0 baseline_failures=0
  tracking_interval_seconds=180 battery_capacity_mah=0
CONFIG V2 CUT AUTHORITATIVE TOKEN unavailable
CONFIG V2 CUT PAGE A evidence=V2_STAGED decoded=yes tail_erased=yes
  generation=1 incarnation=0x55A28DD0B94E1604 revision=1
  tracking_interval_seconds=180 battery_capacity_mah=0
CONFIG V2 CUT PAGE B evidence=ERASED decoded=no tail_erased=yes
CONFIG V2 BASELINE CUT RECOVERY COMPLETE; DO NOT TREAT STAGED TOKEN AS AUTHORITATIVE
```

Therefore the fresh-baseline electrical interruption gate is **PHYSICAL PASS
for the after-body-readback / before-commit cut point**:

- a staged v2 record survives the real power cut as readable evidence;
- it is not promoted to an authoritative token;
- token authority is unavailable to callers;
- runtime stays readable on safe semantics;
- maintenance is required rather than silently re-baselining over staged
  evidence;
- the inactive page remains erased;
- no new baseline commit occurs during recovery.

This is not evidence for every possible electrical interruption instant inside
the NVMC body or commit-word operation. The normal-save qualification below
still requires explicit erase/body/commit boundary coverage.

The staged result was then independently re-read with the separate
`rak4630_config_v2_qual_probe` image after DFU/reset. It reported the same
persistent state:

```text
CONFIG V2 STORE ready=yes maintenance=yes token_state=UNCERTAIN
  committed_override=no baseline_commits=0 baseline_failures=0
CONFIG V2 TOKEN unavailable
CONFIG V2 PAGE A evidence=V2_STAGED decoded=yes tail_erased=yes
  generation=1 incarnation=0x55A28DD0B94E1604 revision=1
  tracking_interval_seconds=180 battery_capacity_mah=0
CONFIG V2 PAGE B evidence=ERASED decoded=no tail_erased=yes
```

This cross-check confirms the observed recovery classification is not dependent
on the power-cut probe's own reporting path. The operator then issued the
qualification helper's explicit ConfigStore-only `CLEAN`; the full 8 KiB
region was read-verified erased again before proceeding.

### Gate 3A — normal-save power cut after inactive-page erase

Starting physical baseline before the cut:

- incarnation `0xE70B5167C5F370E5`;
- revision/generation `1/1`;
- Page A = `V2_COMMITTED`;
- Page B = `ERASED`;
- config = interval 180 s, battery 0.

Test-only target:
`rak4630_config_v2_normal_save_powercut_probe`.

Build evidence from
`test/config-store-v2-physical-qualification@36f5dd2d4cbfbd5817b69e235b3a5670ffa7b81d`:

- build: **SUCCESS**;
- RAM: **8,996 / 248,832 B = 3.6%**;
- Flash: **65,708 / 815,104 B = 8.1%**;
- upload via nrfutil: **SUCCESS**.

The operator issued `CUT_ERASE`. The probe submitted a real
`ConfigStore::requestSave()` candidate changing battery capacity from 0 to 1,
then delegated the real inactive-page erase to `NrfConfigFlash`. After the
erase returned physically complete/read-verified, but before ConfigStore could
program the new v2 body, it reported:

```text
CONFIG V2 NORMAL CUT_ERASE accepted candidate tracking_interval_seconds=180 battery_capacity_mah=1
CONFIG V2 NORMAL CUT READY stage=after-erase-readback before-body; CUT POWER NOW
```

The operator removed physical power and then restored it with the same image.
Production ConfigStore recovery reported:

```text
CONFIG V2 NORMAL BEGIN PASS
CONFIG V2 NORMAL STORE ready=yes maintenance=no token_state=VALID
  committed_override=no busy=no tracking_interval_seconds=180
  battery_capacity_mah=0 saves=0 save_failures=0
  baseline_commits=0 baseline_failures=0
CONFIG V2 NORMAL TOKEN incarnation=0xE70B5167C5F370E5 revision=1
CONFIG V2 NORMAL PAGE A evidence=V2_COMMITTED decoded=yes tail_erased=yes
  generation=1 incarnation=0xE70B5167C5F370E5 revision=1
  tracking_interval_seconds=180 battery_capacity_mah=0
CONFIG V2 NORMAL PAGE B evidence=ERASED decoded=no tail_erased=yes
```

Therefore the normal-save **erase boundary is PHYSICAL PASS**:

- the pre-existing committed semantic config survived the real power cut;
- the candidate config was not falsely applied;
- incarnation/revision/token authority remained exactly on the old committed
  record;
- recovery required no maintenance lockout or baseline rewrite;
- the inactive page remained safely erased.

### Gate 3B — normal-save power cut after body+CRC readback

Starting from the same valid revision-1 lineage after Gate 3A, the operator
issued `CUT_BODY`. The probe submitted the same real normal-save candidate
(battery capacity 0 -> 1). The inactive page was erased, the full 44-byte
body+CRC stage was physically programmed through `NrfConfigFlash`, and
ConfigStore's own stage readback/memcmp completed. The wrapper then withheld the
4-byte commit program and reported:

```text
CONFIG V2 NORMAL CUT_BODY accepted candidate tracking_interval_seconds=180 battery_capacity_mah=1
CONFIG V2 NORMAL CUT READY stage=after-body-readback before-commit; CUT POWER NOW
```

The operator removed and restored physical power. Recovery reported:

```text
CONFIG V2 NORMAL BEGIN PASS
CONFIG V2 NORMAL STORE ready=yes maintenance=no token_state=VALID
  committed_override=no busy=no tracking_interval_seconds=180
  battery_capacity_mah=0 saves=0 save_failures=0
  baseline_commits=0 baseline_failures=0
CONFIG V2 NORMAL TOKEN incarnation=0xE70B5167C5F370E5 revision=1
CONFIG V2 NORMAL PAGE A evidence=V2_COMMITTED decoded=yes tail_erased=yes
  generation=1 incarnation=0xE70B5167C5F370E5 revision=1
  tracking_interval_seconds=180 battery_capacity_mah=0
CONFIG V2 NORMAL PAGE B evidence=V2_STAGED decoded=yes tail_erased=yes
  generation=2 incarnation=0xE70B5167C5F370E5 revision=2
  tracking_interval_seconds=180 battery_capacity_mah=1
```

Therefore the normal-save **body boundary is PHYSICAL PASS**:

- the old committed semantic config remains authoritative;
- the staged successor is preserved as coherent evidence but is not promoted;
- token authority remains exactly on incarnation
  `0xE70B5167C5F370E5`, revision 1;
- the uncommitted candidate does not become the visible application config;
- exact successor lineage (generation/revision +1, same incarnation) survives
  the real power cut;
- recovery stays out of maintenance because the committed page plus exact
  staged successor is an expected recoverable state.

### Remaining audit obligations

Still separate after Gate 3B:

- BLE-connected v2 ConfigStore flash probe plus lineage observation;
- normal-save power cut at the commit boundary.

The original legacy-v1 development evidence was physically observed before the
explicit Gate 1 CLEAN and is recorded above; it must not be described as
untouched after that authorized maintenance erase.

No host/build result may be reported as physical evidence.
