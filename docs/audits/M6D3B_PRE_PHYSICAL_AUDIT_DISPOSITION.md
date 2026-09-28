# M6D3B pre-physical audit disposition

Status: **PASS — READ-ONLY PHYSICAL PREFLIGHT AUTHORIZED; DESTRUCTIVE QUALIFICATION STILL GATED BY PREFLIGHT ALL-FF EVIDENCE.**

Audited exact head: `bf1a753703c9a51381a0e0a31f631dba95a46c3d`.

Baseline: `main@e566125afb9a08aaa5da8656d22d82e6a7580aed`.

## Audit sequence

The initial independent pre-physical review returned **PASS WITH FIXES** and
identified three actionable items:

1. aged Geofence work could still starve behind a stream of fresh
   `SEC_CRITICAL` requests;
2. the destructive qualification image had a credible 4-KiB loop-task stack
   overflow risk on the fresh-baseline path;
3. the forbidden-owner source-contract regex did not reliably match real
   forbidden symbols.

All three were corrected on the same branch and revalidated.

The focused re-audit at the exact head above returned **PASS**.

## Closed findings

### F1 — aged Geofence starvation

Closed.

`higherPriorityWaiting()` now orders requests by:

1. aged tier versus fresh tier;
2. oldest staged request within the aged tier;
3. frozen normal priority as deterministic tie-breaker.

Therefore an aged Geofence request eventually outranks fresh
`SEC_CRITICAL` work. With no aged requests, the normal order remains:

```text
SEC_CRITICAL > History > Config > Geofence > SEC_MAINT
```

Timeout/quarantine/late-completion ownership semantics were unchanged and their
regressions remain PASS.

### F2 — qualification loop-task stack risk

Closed for the audited image.

Record/snapshot diagnostic workspaces were moved out of nested task-stack locals
where appropriate. Focused `-fstack-usage` analysis measured:

- fresh-baseline deepest call chain at approximately **2.8 KiB**;
- STATUS path at approximately **1.5 KiB**.

Both remain below the framework's 4-KiB loop-task stack with about 1.1 KiB
estimated margin on the deeper path.

LOW residual note: several compiler-generated/format-local record-sized frames
remain (roughly 550-650 B each). The measured current image remains within the
stack budget, but future changes to these call chains should preserve stack
measurement discipline.

### F3 — source-contract regex

Closed.

The source guard now:

- ignores forbidden names present only in comments;
- detects a real forbidden symbol such as `PositionFlow* p;`;
- does not falsely match a longer identifier such as `PositionFlowX`.

Self-checking assertions cover this behavior.

## Revalidation evidence

At `bf1a753703c9a51381a0e0a31f631dba95a46c3d`:

- `./firmware/tests/run_host_tests.sh`: **PASS**;
- M6D3B source contract: **PASS**;
- M6D3B GeofenceStore recovery/mutation: **PASS**;
- M6D3B FlashMutationGate geofence client: **PASS**;
- `pio run -e rak4630`: **SUCCESS**, RAM **24,224 B**, Flash **252,748 B**;
- `pio run -e rak4630_m6d3b_geofence_preflight`: **SUCCESS**, RAM
  **8,740 B**, Flash **58,588 B**;
- `pio run -e rak4630_m6d3b_geofence_qual`: **SUCCESS**, RAM
  **14,588 B**, Flash **68,596 B**;
- `git diff --check`: clean.

Production still has no `GeofenceStore` instance and no
`.geofencePort()` use. Production RAM remains 24,224 B.

The preflight ELF contains no linked flash mutation primitive/backend path.
The destructive qualification image does contain the intended GeofenceStore
mutation path.

## Physical evidence boundary

No M6D3B hardware persistence claim exists yet.

Still **NOT RUN**:

- physical read-only flash preflight;
- physical A/B persistence;
- reboot persistence;
- physical electrical power-cut qualification.

## Decision

The read-only preflight image is safe to upload to the intended development
RAK4631 for inspection of:

`0x0E5000..0x0E7000`.

The destructive qualification image remains forbidden until the exact same
development unit reports both geofence pages as:

`all_ff=yes`.

Any non-FF/unknown evidence is a STOP condition and must not be erased
automatically.
