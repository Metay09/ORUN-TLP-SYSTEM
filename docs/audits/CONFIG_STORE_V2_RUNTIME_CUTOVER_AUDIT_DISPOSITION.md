# ConfigStore v2 Runtime Cutover — Independent Audit Disposition

Status: **INDEPENDENT FINAL AUDIT — PASS WITH MINOR FIX; 0 BLOCKER / 0 HIGH; REQUIRED DOC + HOST-TEST FIX APPLIED — 2026-09-26.**

PR:

`#46 — ConfigStore: switch runtime baseline and saves to v2`

Base:

`main@0ad74d6bf2f736ce52cc502b92cc9b92970e053c`

Independent-review head:

`a3c1f0f4fc3bc7f4ce2adeba7fa7b700a4273ecf`

The review covered fresh-baseline creation, nRF52840 incarnation generation,
v2 A/B save/recovery, full-page blank/tail ownership, semantic fallback versus
token authority, GET_CONFIG provenance, legacy-v1 clean cutover,
FlashMutationGate late completion, startup ordering, wear and build impact.

No physical persistence/power-cut validation was performed or claimed.

---

## Verdict

**PASS WITH MINOR FIX**

- BLOCKER: 0
- HIGH: 0
- MEDIUM: 1
- LOW / DOC: 4

The reviewer found no production executable defect requiring a runtime-code
change before merge.

---

## M1 — logical failure may later reconcile as applied

**MEDIUM — accepted; API contract + host regression added.**

A commit-word program can already have reached flash when a later timeout or
final readback failure causes the logical save attempt to return
`takeSaveResult(false)`.

Full partition reconciliation may then prove that the exact successor record is
durably committed and make its config/token authoritative.

Therefore:

```text
takeSaveResult(false)
!=
candidate definitely not applied
```

It means only that the attempt was **not confirmed successful**.

Applied closure:

- `ConfigStore::takeSaveResult()` contract now explicitly states that
  `false` can mean `UNCONFIRMED / OUTCOME_UNKNOWN`;
- protected future CAS/COMMAND RESULT is forbidden from converting that state
  into definitive `FAILED`;
- callers requiring application outcome must re-read coherent state after
  reconciliation;
- host regression now writes the commit word, forces the final record readback
  to fail once, observes logical failure/token UNCERTAIN, then proves full
  read-only recovery selects the new config and revision exactly once;
- recovery never emits a synthetic second application success.

No new production result enum is added in PR #46 because protected config
COMMAND/RESULT is not yet implemented. `OUTCOME_UNKNOWN / UNCONFIRMED` is a
hard prerequisite before that later slice can close.

---

## L1 — one-word ORC1 torn body

**LOW — documented as an accepted fail-closed availability cost.**

If only the first 32-bit `ORC1` word is programmed and all later classifier
words remain erased, current classification may become local
`SUPPORTED_CORRUPT` rather than the normal torn-prefix class.

This never promotes a token or wrong config. The surviving committed semantic
config may remain readable, but token state becomes UNCERTAIN and
maintenance/re-baseline is required.

The behavior is deliberately documented rather than broadening the classifier
in this slice.

---

## L2 — no in-firmware destructive maintenance/re-baseline yet

**LOW / DOC — explicit development procedure and prerequisite recorded.**

PR #46 intentionally does not implement automatic destructive re-baseline.

Until the dedicated maintenance slice exists, development recovery for
legacy/corrupt/unsafe ConfigStore state is:

```text
erase page 0x0E9000..0x0E9FFF
erase page 0x0EA000..0x0EAFFF
verify 0x0E9000..0x0EAFFF == 0xFF
normal reboot -> fresh v2 baseline
```

This operation is ConfigStore-only. It must not touch:

- `0x0E7000..0x0E8FFF` SecurityStore;
- `0x0EB000..0x0ECFFF` BLE bonds/InternalFS;
- `0x0ED000..0x0F3FFF` HistoryStore.

An in-firmware bounded maintenance/re-baseline path is now an explicit
prerequisite before protected CAS mutation is enabled.

---

## L3 — GET_CONFIG bit1 false can accompany non-default fallback values

**LOW / DOC — clarified without wire change.**

`config_has_committed_record=false` / GET_CONFIG bit1=0 means no committed
application semantic override is being claimed.

It does **not** guarantee that returned numeric values equal compile-time
defaults. A verified staged/partial non-authoritative semantic fallback may be
non-default while bit1 remains 0.

M7P7D/M7P7F now state this explicitly. No GET_CONFIG byte changed.

---

## L4 — runtime recovery read failure flag combination

**LOW / DOC — clarified.**

If a reconciliation read fails after a previously committed semantic override
was known, ConfigStore preserves the last semantic config/provenance while
dropping token authority and store readiness.

The following combination is therefore valid:

```text
ready = false
hasCommittedRecord = true
token_state = UNCERTAIN
maintenanceResetRequired = true
```

It means "last known committed semantic config retained, current backend state
not successfully re-observed", not a fresh authoritative storage read.

---

## Independent evidence

The reviewer independently reran:

`bash firmware/tests/run_host_tests.sh`

on the reviewed branch and reported exit 0.

The owner validation on exact review head
`a3c1f0f4fc3bc7f4ce2adeba7fa7b700a4273ecf` recorded:

- full host suite: **PASS**;
- ConfigStore v2 runtime: **PASS**;
- ConfigStore v2 classifier: **PASS**;
- FlashMutationGate / M7P7B flash probe: **PASS**;
- production startup scenarios: **PASS**;
- project warnings-as-errors + ASan/UBSan paths: **PASS**;
- RAK4630 production build: **SUCCESS**;
- RAM: **22,848 / 248,832 B = 9.2%**;
- Flash: **243,900 / 815,104 B = 29.9%**;
- delta versus PR #45 merged main: **+80 B RAM / +5,388 B Flash**.

The post-audit changes described above are documentation/comment/test-only; the
production ConfigStore runtime behavior accepted by the independent review is
unchanged.

---

## Physical validation disposition

The independent reviewer selected:

**Software-complete and safe to merge with physical persistence validation
explicitly deferred before production/hardware qualification and before
protected config mutation is enabled.**

No physical device is currently available, so no physical PASS is claimed.

Minimum future physical closure includes:

1. erased-device baseline dump and cold-boot incarnation verification;
2. power cut during fresh baseline;
3. BLE-connected v2 M7P7B flash probe with flash-lineage dump;
4. normal-save power cuts during erase/body/commit boundaries;
5. legacy-v1 development partition remains unchanged until explicit
   maintenance erase.

---

## Merge closure rule

PR #46 may leave Draft after:

1. the new M1 host regression passes in the full host suite;
2. the documentation changes above are present;
3. final diff confirms no unintended executable production change after the
   independently reviewed head.

Physical persistence qualification remains open and must not be represented as
completed by this merge.
