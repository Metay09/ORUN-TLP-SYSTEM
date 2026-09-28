# M6D3B final audit disposition

Status: **PASS — MERGE READY.**

Baseline:
`main@e566125afb9a08aaa5da8656d22d82e6a7580aed`.

Final disposition branch head before this document:
`cfbe0b698a793921a77c75c797744d4f13c3b8ac`.

Pre-physical independent Astra PASS production-code head:
`bf1a753703c9a51381a0e0a31f631dba95a46c3d`.

Physical power-cut probe executable code head:
`79a8940259d2d6507223ddfa3d66abdc84fd2424`.

## 1. Scope disposition

M6D3B adds the durable GeofenceStore persistence owner and the Geofence client
of the existing single FlashMutationGate, but deliberately does **not** activate
GeofenceStore in production composition.

Production remains operationally unconfigured for geofence persistence until
M6D3C supplies the committed read-only snapshot to the M6D2 runtime seam.

No TLP v1 byte, RF parameter, GNSS behavior, M6D2 confirmation behavior,
History/Config/Security durable format, BLE bond ownership, application GATT
wire contract, or user-visible field behavior is intentionally changed.

## 2. Independent audit lineage

The initial independent pre-physical audit returned **PASS WITH FIXES** for:

1. aged-request starvation behind fresh SEC_CRITICAL work;
2. destructive qualification loop-task stack risk;
3. forbidden-owner source-contract regex weakness.

All three were fixed and the focused Astra re-audit returned **PASS** at
`bf1a753703c9a51381a0e0a31f631dba95a46c3d`.

The durable pre-physical disposition is:

`docs/audits/M6D3B_PRE_PHYSICAL_AUDIT_DISPOSITION.md`.

A compare from that audited production-code head to the final pre-disposition
branch head shows no later production source changes. The post-audit delta is
limited to:

- physical qualification/audit documentation;
- milestone evidence updates;
- one test-only PlatformIO power-cut environment;
- one test-only geofence power-cut probe;
- source-contract assertions for that probe.

Therefore the production executable code reviewed by Astra was not changed
during physical qualification.

## 3. Software/build evidence

At the audited production-code head:

- complete host suite: **PASS**;
- M6D3B source contract: **PASS**;
- M6D3B GeofenceStore recovery/mutation tests: **PASS**;
- M6D3B FlashMutationGate tests: **PASS**;
- warnings / ASan / UBSan validation: **PASS**;
- production RAK4630 build: **SUCCESS**;
- production RAM: **24,224 / 248,832 B (9.7%)**;
- production Flash: **252,748 / 815,104 B (31.0%)**;
- read-only preflight build: **SUCCESS**;
- destructive qualification build: **SUCCESS**;
- application-ceiling / exclusive-owner guards: **PASS**.

After the audit, the test-only power-cut probe was added without changing
production source. Its focused validation was:

- `python3 tests/m6/test_m6d3b_source_contract.py`: **PASS**;
- `pio run -e rak4630_m6d3b_geofence_powercut`: **SUCCESS**;
- warning-free build after the sign-compare cleanup;
- RAM: **14,452 / 248,832 B (5.8%)**;
- Flash: **67,608 / 815,104 B (8.3%)**.

A complete host-suite repetition was not required solely for this post-audit
test-only addition because no production source or existing production build
environment was changed; the affected source-contract test and the new probe
target were re-run directly.

## 4. Physical qualification evidence

Detailed evidence is recorded in:

`docs/audits/M6D3B_PHYSICAL_QUALIFICATION.md`.

On the intended development RAK4631:

1. read-only partition preflight: **PASS**;
2. blank -> committed CLEAR baseline: **PASS**;
3. page A -> page B CONFIGURED successor: **PASS**;
4. reboot recovery selecting the newer committed page: **PASS**;
5. page B -> page A CLEAR rollover: **PASS**;
6. focused real electrical power cut after body+CRC readback and before the
   4-byte commit: **PASS**.

The focused electrical cut recovered as:

- page A: committed CLEAR generation/revision 3, still authoritative;
- page B: staged CONFIGURED generation/revision 4;
- resource: CLEAR;
- token: VALID revision 3;
- incarnation unchanged;
- staged successor not promoted;
- no implicit baseline creation or auto-erase.

This is physical evidence for the intended commit-last boundary. It is not an
exhaustive claim about every possible intra-instruction electrical timing point.

## 5. Ownership / recovery invariants

Final review finds the intended invariants preserved:

- Geofence flash ownership is bounded to `0x0E5000..0x0E7000`;
- one physical flash mutation owner remains in force;
- normal admission order is
  `SEC_CRITICAL > History > Config > Geofence > SEC_MAINT`;
- aged requests enter a tier above all fresh requests and oldest-aged wins;
- accepted asynchronous timeout does not release physical ownership early;
- late definitive SoftDevice completion remains required to reconcile a
  quarantined operation;
- one exact staged successor with erased commit does not replace prior
  committed authority;
- contradictory committed incarnations fail closed;
- unsupported/newer evidence is not auto-erased;
- semantic CLEAR is never inferred merely from erased flash;
- normal semantic mutations advance generation and revision exactly once while
  preserving incarnation.

## 6. Residual notes / explicit limits

These are not merge blockers:

- physical qualification used the SoftDevice-disabled synchronous geofence
  backend path; GeofenceStore is not yet production-instantiated, so no claim
  is made that a real production geofence mutation has been physically
  exercised through the SoftDevice-enabled gate;
- the focused power-cut probe covers the architecturally important
  after-body-readback / before-commit boundary, not every possible power-loss
  instant;
- the qualification image's current measured stack margin must be rechecked if
  future record-sized locals are added to the same call chains;
- geofence persistence is still not user-visible product behavior until M6D3C
  connects the durable snapshot into the M6D2 runtime provider.

## 7. Decision

**PASS — M6D3B is merge ready.**

The implementation is consistent with the existing storage ownership model,
preserves production activation boundaries, has focused host/build coverage,
has independent pre-physical audit PASS, and now has real A/B, reboot and
commit-last electrical power-cut evidence.

No additional physical action is required for M6D3B before merge.
