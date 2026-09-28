# M6D3 Geofence Resource Foundation — Focused Architecture Audit Disposition

Status: **FINAL FOCUSED VERIFY PASS WITH MINOR DOC FIX; ALL PRIOR FINDINGS FIXED; 0 BLOCKER / 0 HIGH / 0 MEDIUM — 2026-09-28**

Baseline:
`main@34881858817f4e6bfc75c706f4a9d495e47b25d4`

Initial reviewed head:
`5c08c5847ca8167b2e6f1f88007e847c1fdded3d`

Primary source:
`docs/milestones/M6D3.md`

This was a documentation/architecture review only. No firmware build, host test
or physical test was part of the initial audit.

## Accepted architecture

The reviewer accepted:

- first production capacity: **8 areas / 64 total effective vertices**;
- existing 64-effective-vertex per-polygon geometry ceiling;
- dedicated **`0x0E5000..0x0E7000`** GeofenceStore reservation;
- two-page A/B whole-resource persistence;
- separate opaque **96-bit geofence CAS token** rather than ConfigStore token reuse;
- whole-resource `REPLACE_SET` / `CLEAR_SET`;
- M6D3A format/classifier-first sequencing before any writer exists;
- continued single physical-flash arbiter / single SoftDevice event-owner rule;
- deferring exact GeofenceStore FlashMutationGate priority until M6D3B.

No composite ConfigStore+GeofenceStore transaction is required by the current
product requirements.

## H1 — build guard must really move to 0x0E5000

**Accepted / correction applied to M6D3A acceptance criteria.**

The current build script parses `kFutureSecurityRegionStart`, so changing only
`kApplicationPolicyEndAddress` would leave the real post-link ceiling at the
old `0x0E7000` boundary.

M6D3A is now required to:

- introduce literal `kGeofenceRegionStart = 0x0E5000`;
- enforce `kGeofenceRegionEnd == kFutureSecurityRegionStart`;
- enforce `kApplicationPolicyEndAddress == kGeofenceRegionStart`;
- make the build script derive/check the new lowest reserved boundary;
- fail closed if the symbol/chain cannot be parsed;
- include a pure helper regression proving the ceiling is exactly `0x0E5000`;
- update stale build-guard diagnostics/comments.

This is a code-slice merge gate, not optional cleanup.

## Medium findings

### M1 — stale layout sources

**Accepted / docs corrected.**

`ADR_M7_PERSISTENCE_LAYOUT.md` and
`ORUN_STORAGE_FLASH_OWNERSHIP.md` now carry explicit supersession notes for
the proposed geofence region and new ceiling. The reset table is extended with
the geofence resource.

### M2 — CLEAR versus unavailable authority

**Accepted / contract corrected.**

Recovered resource state is now explicitly:

- CLEAR;
- CONFIGURED;
- UNAVAILABLE / maintenance.

Corrupt/unsupported/ambiguous state is never inferred as CLEAR and cannot return
`ALREADY_SATISFIED` for CLEAR. Normal ConfigStore reset and BLE unpair preserve
geofence state; factory reset establishes authoritative CLEAR under a fresh
geofence incarnation.

### M3 — byte-format preconditions

**Accepted / decisions frozen before M6D3A.**

- only effective vertices are durably encoded; explicit closing duplicate is
  canonicalized away;
- physical generation is separate from semantic revision;
- canonical equality is ordered canonical-byte/field identity, not geometric
  equivalence or polygon sorting;
- conflicting committed incarnations fail closed instead of selecting by
  generation alone.

### M4 — completed representative must not be discarded on config change

**Accepted / publication contract corrected.**

REPLACE/CLEAR may discard unfinished confirmation evidence, but a representative
already selected by a completed episode is a real accepted POSITION fact and
continues through the existing PositionFlow store-before-send path.

### M5 — DFU budget

**Accepted / arithmetic recorded without overclaiming.**

A hypothetical equal two-bank model below the new ceiling gives
391,168 bytes per bank. The real RAK4631 bootloader bank/protection behavior
remains unverified and must be physically/externally established before BLE DFU
is considered qualified with the added reservation.

## Low findings

- reboot-resumable fragment staging is explicitly outside the two committed A/B
  pages; adding durable staging later requires separate capacity review;
- wear arithmetic is recorded for M6D3B as engineering guidance, not a physical
  lifetime guarantee;
- M6D3B must read/classify the development unit's proposed region before the
  first destructive writer test and must not erase unknown non-FF contents;
- one semantic `configured` state is used; no second independent enabled bit is
  introduced.

## Physical evidence boundary

M6D3A remains codec/layout/classifier-only. It requires host/sanitizer coverage
and a production RAK4630 build with the corrected application-ceiling guard, but
no hardware test. Those results must not be called physical persistence or
power-cut PASS.

M6D3B is the first slice that may perform real GeofenceStore flash mutation and
therefore owns the read-only preflight plus focused physical persistence work.

## Merge disposition

The documentation branch may merge after focused verification confirms these
corrections. M6D3A implementation must start only from the corrected contract.


## Final focused verification

Focused verification of the corrective documentation returned
**PASS WITH MINOR DOC FIX**.

All prior H1, M1-M5 and L1-L4 findings were confirmed fixed. No new
BLOCKER/HIGH/MEDIUM finding was reported. The remaining required documentation
fixes were:

- update stale `FOCUSED VERIFY PENDING` status text;
- state explicitly that `ALREADY_SATISFIED`/CAS admission requires a VALID
  geofence token; UNCERTAIN authority must surface STATE_UNCERTAIN rather than
  treating matching semantic bytes as authoritative equality.

Both are applied in the final docs head. The M6D3 architecture contract is now
sufficiently frozen to begin M6D3A. This is still documentation evidence only;
no firmware or physical persistence behavior is claimed by this audit.
