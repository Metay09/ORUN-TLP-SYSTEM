# M7P7I Final Audit Disposition

Status: **PASS WITH FIXES — ACCEPTED FIXES APPLIED; OWNER REVALIDATION PENDING.**

PR: #63 — `M7P7I: add accepted Location runtime owner`

Audit baseline:
`main@65916d9e6df333be0fefb9b355ba618e1b1db0de`

Audited code-bearing head:
`4ccc4b1a602111a20345510f2afefc2120edc64d`

Current post-fix branch head:
`f0aee33d98f57720bedbf124ba630d2b472f413a`

## Independent audit result

- BLOCKER: 0
- HIGH: 0
- MEDIUM: 1
- LOW: documentation/test hardening findings
- Verdict: **PASS WITH FIXES**

The audit independently reran the full host suite successfully and performed a
scratch differential TickMillis stress test. Those are host/software evidence,
not physical hardware evidence. The auditor did not rerun PlatformIO; the owner
RAK4630 production-build result remains the build evidence.

## Accepted MEDIUM finding

### M1 — 2D GNSS fix must not claim trustworthy altitude

The initial implementation set `altitude_valid=true` for every promoted
`GnssFix`, based on the fact that GnssManager rejects NAV-PVT
`invalidLlh`.

That was too strong. GnssManager accepts fix type 2 (2D). A 2D solution can
carry an assumed or retained NAV-PVT height while `invalidLlh` remains clear.

Disposition:

- `altitude_mm` remains NAV-PVT `height`, WGS84 ellipsoid height in mm;
- `altitude_valid` now maps from the existing
  `tlp::kPositionFlag3dFix` bit;
- accepted 2D observations keep valid latitude/longitude but expose altitude as
  unavailable/untrusted;
- no TLP v1 packet bytes or GNSS admission rules changed.

## LOW findings disposition

The same branch also addresses the cheap LOW items:

- documents the WGS84 ellipsoid altitude datum;
- documents `extendRecentMonotonicMs()` preconditions;
- adds ordinary and zero-age static checks in addition to rollover coverage;
- strengthens the source contract with exactly-one publication and ordering
  guards;
- ensures the representative storage path cannot republish Location;
- uses an existing startup scenario to exercise 2D
  `altitude_valid=false` while other scenarios exercise 3D
  `altitude_valid=true`;
- tightens accepted observation-time checking from `<= test_now` to equality;
- synchronizes architecture status text.

The audit also noted two useful structural facts which remain explicit guards:
Location publication precedes normal `PositionFlow::acceptFix()`, and
`LocationOwner` has no clear/persistence API. Therefore a later
HistoryStore/PositionFlow failure cannot retract an already accepted runtime
Location in this slice.

## Physical-test disposition

The independent audit found no concrete new hardware-risk path requiring a
dedicated M7P7I physical qualification:

- GNSS acquisition and fresh-fix admission predicates are unchanged;
- no second parser/queue read was added;
- the new owner update is synchronous O(1) RAM work;
- no RF, storage, BLE or GNSS-power behavior changed;
- M7P7I has no product-facing Location reader yet.

Therefore no new M7P7I physical PASS is claimed or required for this slice.
A later routine firmware/GET_LOCATION physical cycle may observe the integrated
behavior, but host/build evidence must not be relabeled as physical evidence.

## Remaining merge gates

After the accepted audit fixes:

1. complete host suite: **PASS** on
   `7d203a3f65e49e013b6bd0e06b6147db88bd2073`;
2. rerun the production RAK4630 build and record RAM/Flash;
3. verify no new audit finding was introduced;
4. mark PR #63 merge-ready and merge.

TLP v1 compatibility/golden fixtures must remain unchanged.
