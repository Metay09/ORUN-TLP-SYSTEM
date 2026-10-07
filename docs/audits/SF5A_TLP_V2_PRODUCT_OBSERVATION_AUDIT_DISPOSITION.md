# SF5A TLP v2 Product Observation Cutover — Independent Audit Disposition

Status: **FINAL PASS — initial PASS WITH FIXES findings F1-F9 are CLOSED;
focused independent re-review returned PASS with 0 BLOCKER / 0 HIGH / 0 MEDIUM /
0 LOW and FINAL RECOMMENDATION: MERGE.**

Audit target:
`pr81@8e82373c50bc02b1053d48e46dd45d32ee741ad2`

Base:
`main@89492c8d87b42fe1c9bd6bb334b41812640db74b`

Fixes applied through:
`a288e8d3f233e1b1ac7aa99136bc61fc9527fd7d`
before this disposition record was added.

Initial verdict:

```text
VERDICT: PASS WITH FIXES
BLOCKER: 0
HIGH:    2
MEDIUM:  6
LOW:     1
FINAL RECOMMENDATION: FIX THEN RE-REVIEW
```

The review was documentation/repository consistency only. It did not run a
production build or physical test and did not claim runtime, RF, power, flash,
DFU or end-to-end validation.

## F1 — Reset-safe tracker responsibility release

Severity: **HIGH**

Finding:
SF5 required exact-object custody ACK mapping but did not require bounded durable
tracker release state after responsibility transfer. Reset could therefore make
previously transferred backlog appear wholly unreleased and allow repeated
re-protection into opaque Gateway custody.

Disposition: **FIXED**

Applied contract:

- ObservationStore must own/reference bounded reset-safe responsibility-release
  state.
- Out-of-order custody ACKs must be representable safely, or scheduling must be
  constrained so the durable representation remains correct.
- A single contiguous watermark is not assumed sufficient.
- RAM-only progress may cause a safe duplicate after reset, but SF5B/SF5C must
  quantify and test the maximum duplicate re-protection/replay amplification per
  record and reset/checkpoint cycle.
- SF5C explicitly carries this as a persistence/fault-injection gate.

No byte layout or checkpoint frequency is frozen by SF5A.

## F2 — 256 KiB reservation vs DFU bank model

Severity: **HIGH**

Finding:
The candidate `0x0A5000..0x0E5000` ObservationStore reservation moved the
application ceiling without making the unresolved DFU bank/update model an
explicit pre-layout gate.

Disposition: **FIXED**

Applied contract:

- SF5D must resolve the DFU bank/update model before freezing the 256 KiB
  reservation/application ceiling.
- The candidate application range is documented as
  `0x026000..0x0A5000 = 520,192 B`.
- A simple equal two-bank split would be about 260,096 B per bank, below the
  current SF4B production image measurement of 285,924 B.
- This arithmetic is evidence of a layout/DFU dependency only; it does not claim
  which DFU model ORUN currently uses or must use.

## F3 — Configured vs effective report period

Severity: **MEDIUM**

Finding:
Several SF5A sentences still described PERIODIC cadence as the configured/base
period, conflicting with the existing adaptive effective cadence such as
geofence OUTSIDE `B/3`.

Disposition: **FIXED**

Applied contract:

- PERIODIC represents one **effective** report period selected by Tracking
  policy.
- Activity summary covers that same effective interval.
- Capacity/RF analysis must use the shortest effective cadence an approved
  runtime policy can actually select.
- SF5F cutover language now also uses effective period.

## F4 — RESULT retry amplification

Severity: **MEDIUM**

Finding:
The first SF5A wording could be read as requiring a new durable RESULT record for
every authenticated cryptographic retry, conflicting with delegated-command/CAS
RESULT-loss retry semantics.

Disposition: **FIXED**

Applied contract:

- Durable RESULT ownership is per logical command in its authenticated authority
  context, not per security-counter retry.
- Repeating the same `command_id` with the same logical payload/target must not
  append an unbounded series of RESULT records.
- Retained logical RESULT is reused where available.
- The first ConfigStore desired-state family may reconstruct
  `ALREADY_SATISFIED` from reviewed durable CAS/application state after RESULT
  loss without another application-effect flash write.
- SF5B/SF5C must define bounded RESULT retention/deduplication without inventing
  a generic persistent command-ID journal.

## F5 — EVENT occurrence identity across reset/ring rotation

Severity: **MEDIUM**

Finding:
ACTIVE/CLEARED identity semantics were defined, but the minimum open occurrence
state could have been implemented as RAM-only or lost when the historical EVENT
record aged out of the ring.

Disposition: **FIXED**

Applied contract:

- Minimal open-occurrence identity/state is durable control state.
- It survives reset and is not lost merely because historical backlog rotates.
- One owner must retain enough state to emit the matching CLEARED transition or
  perform explicit fail-closed reconciliation.
- Recovery uncertainty must not silently mint a second ACTIVE identity.

Concrete owner/bytes remain SF5C work.

## F6 — Current runtime incorrectly described as v1-only

Severity: **MEDIUM**

Finding:
Current runtime already includes provisioned SF3 TLP v2 HISTORY_SECURE replay in
addition to frozen TLP v1 live POSITION/relay behavior.

Disposition: **FIXED**

Applied contract:

- AGENTS and SF5 architecture now state the current two-path evidence boundary:
  frozen v1 live POSITION/relay plus provisioned SF3 v2 HISTORY_SECURE replay.
- SF2/SF3 HISTORY_SECURE bytes/vectors remain frozen compatibility evidence.
- SF5B must define a new protected product-observation family rather than
  widening/reinterpreting the current 73-byte HISTORY_SECURE family.
- At SF5F direct cutover, the new SF5 family replaces HISTORY_SECURE for new
  product observations and the legacy SF3 replay runtime is retired together
  with legacy HistoryStore unless a separately approved migration requirement
  exists.
- Legacy development backlog is not silently reported as migrated/preserved.

## F7 — Offline read authority vs origin forgery

Severity: **MEDIUM, non-blocking in initial review**

Finding:
A future offline read/decrypt grant must not accidentally require giving a phone
or Edge the same symmetric authority needed to forge tracker-originated traffic.

Disposition: **FIXED EARLY / PROMOTED TO SF5B GATE**

Applied contract:

- SF5B must preserve a path for offline read authority that does not imply
  tracker-origin authentication/forgery authority.
- If a proposed symmetric construction cannot provide that separation, the
  limitation must be an explicit reviewed security decision/blocker before wire
  freeze.
- No new cryptographic construction is selected by SF5A.

## F8 — Durable owner transition semantics

Severity: **MEDIUM, non-blocking in initial review**

Finding:
Selecting ObservationStore vs CustodyStore by product configuration did not yet
state how an existing non-empty owner region may transition safely.

Disposition: **FIXED EARLY / PROMOTED TO SF5D GATE**

Applied contract:

- Profile/capability change is not implicit erase/migration authority.
- A non-empty durable region may change owner only through an explicit
  maintenance transition.
- Unresolved accepted Gateway custody blocks transition.
- Old responsibility must be drained/closed, or data loss must be an explicit
  destructive re-baseline decision.
- Legacy Role must not select the owner.

## F9 — Activity coverage had an implicit gap bucket

Severity: **LOW**

Finding:
`coverage_seconds + unknown_seconds <= period_duration_seconds` allowed time to
be neither covered nor explicitly unknown.

Disposition: **FIXED**

Applied invariant:

```text
coverage_seconds + unknown_seconds == period_duration_seconds
```

for a finalized PERIODIC record. Any interval without usable activity evidence
is explicitly unknown.

## Additional consistency fixes

The same fix pass also keeps these previously verified invariants explicit:

- storage format != wire format;
- Role != Capability != Transport != Identity != Security Authority;
- RF/service priority != storage eviction != Gateway FIFO durable drain;
- Gateway accepted custody is never pressure-evicted;
- current SF4B CustodyStore geometry remains bound to the 73-byte
  HISTORY_SECURE object and must be versioned/re-reviewed if SF5 protected
  objects differ;
- no unauthenticated/malformed/replay-rejected command traffic may create
  durable RESULT spam;
- v1 golden fixtures and SF2/SF3 HISTORY_SECURE frozen vectors are not weakened.

## Evidence boundary

This disposition records documentation fixes only.

It does **not** prove:

- SF5 runtime cutover;
- ObservationStore implementation;
- 256 KiB physical ownership;
- DFU behavior/model;
- power-cut or flash-wear behavior;
- RF airtime/duty-cycle/collision capacity;
- real battery/power consumption;
- cryptographic correctness of a future SF5B envelope;
- Gateway custody runtime;
- Tracker -> Gateway -> Edge -> Backend -> App end-to-end behavior.

Focused independent re-review of
`pr81@d16d118e1ac486f94b7fa8c02012d0fb28c277e0` returned:

```text
VERDICT: PASS
BLOCKER: 0
HIGH:    0
MEDIUM:  0
LOW:     0
FINAL RECOMMENDATION: MERGE
```

The reviewer confirmed F1-F9 CLOSED, found no new finding, and reiterated the
same evidence boundary: documentation/repository consistency only; no build,
runtime, DFU, flash, RF, power or end-to-end physical proof.
