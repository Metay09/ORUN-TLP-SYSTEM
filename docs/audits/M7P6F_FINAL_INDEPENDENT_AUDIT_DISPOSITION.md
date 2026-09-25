# M7P6F final independent audit disposition

Status: **PASS WITH FIXES — fixes applied on branch; revalidation and physical sentinel pending.**

Audited head: `753965361a865fb823bd691e36df1bcd0b89163f`.

The independent reviewer re-ran the full host suite at the audited head
(EXIT=0), reviewed the baseline diff and production FlashMutationGate paths,
and used additional scratch probes. No code was modified by the reviewer.

## Verdict

- BLOCKER: none
- HIGH: none
- security conclusion: no TX nonce-reuse path demonstrated; no A2D counter
  double-acceptance path demonstrated
- merge conclusion: **PASS WITH FIXES**

The original H1, M1, M2, M4 and L1 fixes were independently confirmed. M3
remains an explicitly accepted fail-closed availability residual. L2/L4 remain
documented wear considerations, L3 remains deferred/non-blocking, and L5 remains
a provisioning/authority invariant.

## New findings and disposition

### N1 — MEDIUM — persistent dirty mutation can cause erase storm

Audited behavior: a persistent "physical write occurred but operation/verify
reported failure" condition could repeatedly burn append slots, enter
compaction, then retry erase/new-page work on every poll. The reviewer observed
3269 erases in 10,000 polls in a probe.

Disposition: **FIXED ON BRANCH; REVALIDATION PENDING.**

SecurityStore now keeps a boot-scoped consecutive security-mutation failure
budget. Three consecutive reserve/replay/new-page mutation failures enter
`SecurityState::kFault` until reboot. Any successful reserve, replay reserve or
new-page activation resets the streak. This preserves same-boot recovery from
isolated dirty writes while bounding persistent-fault wear.

Regression coverage includes:
- repeated dirty append failures;
- active page already at compaction threshold;
- bounded maximum of three repeated compaction erases before FAULT.

### N2 — LOW — failed target inspection could create a durable log gap

Audited behavior: if `fail()` could not read the failed append target, it
conservatively advanced the RAM slot index. If the target was actually still
erased and a later slot committed, reboot would see an erased gap followed by a
record and fail closed permanently.

Disposition: **FIXED ON BRANCH; REVALIDATION PENDING.**

An unreadable append target is no longer skipped. Protected security service
enters FAULT for the boot. Reboot recovery decides from durable bytes.

### N3 — LOW — partial old-page erase may classify as UNSUPPORTED

Audited behavior: a partial erase can turn the stale page's version byte from
v2 into a future value, causing recovery to return UNSUPPORTED rather than
FAULT.

Disposition: **ACCEPTED FAIL-CLOSED RESIDUAL; PHYSICAL SENTINEL PENDING.**

The physical sentinel accepts either:
- new higher generation PROVISIONED, or
- fail-closed FAULT / UNSUPPORTED.

It must never accept an older generation as PROVISIONED or recover a lower
TX/A2D durable bound.

### N4 — LOW — record commit could be written before body readback

Audited behavior: on the asynchronous FlashMutationGate path, a reported body
completion could proceed directly to programming the record commit word, with
the full readback check only afterward. A body mismatch could therefore leave a
committed malformed record and permanent recovery FAULT.

Disposition: **FIXED ON BRANCH; REVALIDATION PENDING.**

`writeBlob()` now read-verifies the complete record body before programming the
commit word. A body mismatch therefore leaves the commit erased and the record
burnable.

The real SecurityStore + FlashMutationGate integration test is also extended
with an append-body timeout / late-success scenario before the existing page
activation ambiguity scenario.

## Remaining evidence gates

Before merge:

1. fresh full host suite, including warnings-as-errors and ASan/UBSan;
2. fresh RAK4630 production build and RAM/flash report;
3. independent review reconciliation of the N1/N2/N4 fixes if required by the
   milestone process;
4. controlled real-RAK4631 M3/power-cut sentinel.

Host/build success must not be described as physical flash/power-cut evidence.
