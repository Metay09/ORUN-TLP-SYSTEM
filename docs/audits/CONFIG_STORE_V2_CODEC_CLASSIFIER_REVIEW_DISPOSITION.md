# ConfigStore v2 Codec / Page Classifier Review Disposition

Status: **INDEPENDENT REVIEW — PASS WITH FIXES; 0 BLOCKER / 0 HIGH; REQUIRED DOC FIXES APPLIED — 2026-09-26.**

PR:

`#45 — ConfigStore: add v2 codec and page classifier`

Baseline:

`main@d460eff63582d5be3f8c51e8da2a75f85c70b37d`

Independent review head:

`c6100348bebcda01241a2e06d951f2a2116ca6f4`

The review covered the exact v2 byte layout, legacy-v1 byte regression safety,
partial-commit/retire evidence, torn-prefix classification, forward-schema
discrimination, structural-vs-semantic ownership and the clean development
cutover scope.

---

## Verdict

**PASS WITH FIXES**

- BLOCKER: 0
- HIGH: 0
- MEDIUM: 2, documentation-only
- LOW / DOC: 3

The reviewer found no executable defect requiring a code change.

The 48-byte v2 codec, CRC/commit/retire offsets, v1 byte compatibility,
partial-commit behavior, retire-word fail-safe classification and torn
program/erase behavior were accepted.

---

## Independent evidence

The reviewer independently checked:

- golden v2 CRC over bytes 0..39: `0xC4596094`;
- 11 fixed + 200,000 randomized partial-commit patterns;
- 200,000 randomized non-FF retire values;
- 20,000 interrupted-program simulations each for v1 and v2;
- 200,000 interrupted-erase simulations each for v1, v2 and retired-v2;
- failed decode paths leave output objects unchanged;
- full host suite PASS;
- production RAK4630 build SUCCESS;
- linked production image contains only currently referenced v1 config-format
  symbols, so v2 codec/classifier is dead-stripped from production.

No physical flash/power-cut/RF evidence is claimed by this slice.

---

## M1 — clean-cutover policy remained contradicted by old migration wording

**MEDIUM / documentation-only — accepted and fixed.**

The runtime target is now unambiguous:

```text
no deployed ConfigStore fleet
development v1 data not guaranteed
-> explicit ConfigStore maintenance/reset
-> blank partition
-> fresh v2 baseline
-> v2-only normal writes
```

Corrections applied:

- §13.4 no longer authorizes automatic v1 migration;
- committed v1 observed by a v2 runtime => maintenance/reset required;
- mixed committed v1+v2 => maintenance/reset required;
- legacy v1 + staged/partial v2 => maintenance/reset required;
- §2.3 migration generation wording is contingency-only;
- wear and implementation-order wording marks legacy migration as contingency;
- `config_format.h` terminology uses runtime **cutover**, not runtime migration.

The previously reviewed migration design remains historical/contingency evidence
only and is not a current implementation gate.

---

## M2 — future schema could look like a torn prefix when final classifier word is erased

**MEDIUM / documentation-only — accepted and fixed.**

Because torn-prefix evidence is intentionally evaluated before
`UNSUPPORTED_NEWER`, a future valid schema that leaves page bytes 48..51 erased
could be conservatively classified as local torn/corrupt evidence.

The forward-compatibility contract now requires every deployable future ORC1
schema that relies on this classifier to satisfy:

```text
magic == ORC1
bytes[5..7] == 0
(version & 0x03) == 0
page bytes[48..51] != 0xFFFFFFFF for every valid non-torn record
```

This requirement is recorded both in the architecture document and adjacent to
`inspectPagePrefix()`.

Future schema designers must change the classifier contract in a separately
reviewed migration before violating this invariant.

---

## L1 — version 0

**LOW — documented, no executable change.**

Version `0` is explicitly reserved as an incompatible/fail-closed sentinel.
It may classify as unsupported/incompatible but is not a deployable future schema
version.

Deployable future versions begin in the reserved multiple-of-four namespace,
for example 4, 8, 12.

---

## L2 — structural vs semantic evidence

**LOW / DOC — accepted and fixed.**

The architecture now maps structural classifier output to the later
ConfigStore semantic/recovery classes.

In particular:

- `kLegacyV1Committed` is structural; semantic failure maps to
  `V1_COMMITTED_INVALID`;
- `kV2Committed` / `kV2CommittedRetired` are structural; semantic failure
  maps to `V2_COMMITTED_INVALID`;
- `kV2Staged` with invalid semantic config maps to
  `V2_UNCOMMITTED_OR_TORN`;
- `kV2PartialCommit` with invalid semantic config maps to
  `SUPPORTED_CORRUPT`.

`PageInspection::has_decoded_record` is explicitly documented as structural
decode evidence only. It never means semantic validity, application authority or
token VALIDity.

---

## L3 — optional wider deterministic test sweep

**LOW / optional — not applied in this PR.**

The reviewer suggested extra deterministic loops for:

- more partial-commit values;
- more retire values;
- full 0..255 version sweep;
- programmed -> erased -> programmed negative torn-prefix pattern.

The existing code behavior was independently stress-probed and accepted, and the
full host suite already passes. Because this finding is optional and would alter
test logic after the validated head without closing an executable defect, it is
deferred rather than churned into this codec-only slice.

A later classifier-test-hardening change may add it without changing runtime
semantics.

---

## Validation evidence

Owner validation on executable/test-logic head:

`766e896d6e691e880ce22a05fd181b0d8c827ab7`

- `bash firmware/tests/run_host_tests.sh`: **PASS**
- new `ConfigStore v2 format/classifier checks: PASS`
- warnings-as-errors + ASan/UBSan host coverage: **PASS**
- all production startup host scenarios: **PASS**
- `pio run -d firmware -e rak4630`: **SUCCESS**
- RAM: **22,768 / 248,832 B = 9.1%**
- Flash: **238,512 / 815,104 B = 29.3%**
- production-image delta vs current production baseline: **+0 B RAM / +0 B Flash**

Changes after that validated executable head are documentation/comment-only and
do not change executable code or test logic.

---

## Closure

The required M1/M2 corrections are applied.

L1/L2 are clarified without executable changes.

L3 remains an optional test-hardening item and is not a merge blocker.

PR #45 may leave Draft and be squash-merged after confirming the final diff
contains only the reviewed codec/classifier implementation plus these
documentation/comment corrections.

This disposition does not authorize the later v2 ConfigStore runtime cutover and
does not claim physical flash/power-cut validation.
