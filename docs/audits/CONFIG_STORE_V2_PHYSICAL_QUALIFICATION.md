# ConfigStore v2 Physical Qualification

Status: **TEST TOOLING PREPARED; PHYSICAL EVIDENCE PENDING**.

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

### Remaining audit obligations

Still separate after Gate 1:

- power cut during fresh baseline;
- BLE-connected v2 ConfigStore flash probe plus lineage observation;
- normal-save power cuts at erase/body/commit boundaries;
- legacy-v1 development evidence remains untouched until explicit CLEAN.

No host/build result may be reported as physical evidence.
