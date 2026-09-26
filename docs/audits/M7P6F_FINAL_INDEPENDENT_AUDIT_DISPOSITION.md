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

SecurityStore keeps a boot-scoped consecutive security-mutation failure
budget for ordinary clean/verify failures. Three consecutive
reserve/replay/new-page mutation failures enter `SecurityState::kFault` until
reboot, and any successful reserve, replay reserve or new-page activation resets
that streak. An accepted SoftDevice mutation that times out while
FlashMutationGate still quarantines the physical request is a stricter case:
it now causes immediate FAULT-until-reboot rather than being retried while the
gate still owns the flash token.

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

The real SecurityStore + FlashMutationGate integration test covers an
append-body timeout / late-success scenario before the existing page-activation
ambiguity scenario. Under the explicit timeout policy, an unreconciled accepted
mutation faults SecurityStore for the boot; repeated normal loop polls while the
gate remains quarantined must not manufacture extra failures or flash writes,
and a late SUCCESS may release physical ownership but must not revive same-boot
SecurityStore authority.

### N1-R — MEDIUM — quarantined async timeout looked like repeated failures

A second independent reconciliation at `8a3c470` found that the first N1
circuit breaker could be tripped by one real SoftDevice timeout: while the
accepted operation remained quarantined, normal loop polls attempted a fresh
reserve, the gate rejected it without a new physical mutation, and those
logical rejections were counted as additional mutation failures.

Disposition: **FIXED ON BRANCH; REVALIDATION PENDING.**

Chosen product policy is deliberately conservative and explicit:

- an ordinary clean/verify mutation failure may use the three-failure
  boot-scoped breaker;
- an async mutation that was physically accepted but times out with
  unreconciled gate ownership causes immediate SecurityStore FAULT until reboot;
- repeated loop polls cannot add synthetic failures or new writes after that
  FAULT;
- a late SUCCESS/ERROR only reconciles FlashMutationGate ownership; it never
  revives SecurityStore authority in the same boot;
- reboot performs authoritative recovery from durable bytes.

One bounded exception is intentional: a timeout while erasing the already
superseded old page runs through `completeEraseOld(false)`, not `fail()`.
The newly activated page is already authoritative, so the store may remain
PROVISIONED if the late erase completion reconciles before any later security
mutation is requested. If another security mutation is attempted while that
erase is still quarantined, the gate rejects it without a physical write and
SecurityStore then enters FAULT through the same unreconciled-mutation guard.
This is an availability distinction only; it cannot restore old authority or
lower a durable TX/A2D bound.

This avoids both the original erase storm and the misleading "three failures"
semantics for one quarantined physical request.

## Remaining evidence gates

Before merge:

1. fresh full host suite, including warnings-as-errors and ASan/UBSan;
2. fresh RAK4630 production build and RAM/flash report;
3. independent review reconciliation of the N1/N2/N4 fixes if required by the
   milestone process;
4. controlled real-RAK4631 M3/power-cut sentinel.

Host/build success must not be described as physical flash/power-cut evidence.
