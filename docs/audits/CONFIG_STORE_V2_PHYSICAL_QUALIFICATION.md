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

### Remaining audit obligations

Still separate after Gate 2:

- BLE-connected v2 ConfigStore flash probe plus lineage observation;
- normal-save power cuts at erase/body/commit boundaries.

The original legacy-v1 development evidence was physically observed before the
explicit Gate 1 CLEAN and is recorded above; it must not be described as
untouched after that authorized maintenance erase.

No host/build result may be reported as physical evidence.
