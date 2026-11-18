# KQ05 Tracker selective, durable release of authenticated Gateway batch ACK

**Status:** DRAFT — PC-only test pending. No production RF/physical Flash.  
**Parent:** PR #348 at `fdbe40e31c15e5ea9c74506e7b8e6802364959ad`. The
device owner verified its independent Python HKDF, real
`SecurityStore V3`, full 1..5 exact-digest HMAC and Tracker five-record
OBS1 durable cache tests as **PC PASS**.

## Safety boundary implemented

The new `Kq05TrackerBatchReleaseOwner` accepts only a complete
raw, canonical Gateway-local v2/type 0x0A ACK and calls
`Kq05TrackerBatchAckVerifier::verifyFromStore()` against the real
Tracker SecurityStore V3 and retained exact OBS1 control objects
**before permitting ANY Flash release marker**. The opaque in-process
proof holds the full SHA256 of each exact frame and corresponding
logical incarnation/sequence, all bound to a single
Tracker/Gateway/key epoch/grant floor/generation.

After complete group authentication, the loop-owned state machine:
1. Checks the committed live Tracker security grant **before each**
   individual release.
2. Re-finds each exact, still-retained encrypted object in
   ProductRecordStore and requires a byte-identical cached object
   and a constant-time comparison with its previously authenticated
   **full 32-byte digest**.
3. Uses the **existing private**
   `ProductRecordStore::requestAuthenticatedCustodyRelease(exact)`
   to request exactly **one** OBS1 durable release marker.
4. Waits for `ProductRecordStore::poll()` (driven by the already existing
   single Flash owner) and consumes `takeCustodyReleaseOutcome()`;
   it MUST NOT advance to any other identity without an exact,
   successful durable marker outcome.
5. Yields one `kReleasedOne` event after every success. Reports
   `kApplied` only after the whole group's marker results are durable.
   New unrelated live telemetry can be scheduled between markers.
6. If the grant is revoked or rotated, the full digest changes, the
   control record becomes unreadable, a marker fails, or a store
   mutation is ambiguous, the owner fails closed. Completed earlier
   markers remain committed; later markers NEVER start by inference.

Only this authenticated owner is made a friend of the private
ProductRecordStore marker API. No public record-ID-based deletion or
radio/ISR-level release entrypoint is introduced.

### Host test

`bash tools/ci/run_kq05_tracker_selective_batch_release_host.sh`

Uses **actual** SecurityStore V3 provision/migration/grant fixture,
ProductRecordStore, ObservationStore release-marker state machine,
OpenSSL implementation of existing HKDF/SHA256/HMAC primitive
contracts, and a fake NOR backend. GNU++11 host build.

Tests:
- Authentication missing/bad MAC/truncated ACK **cannot** start release;
  1..5 complete/subset ACKs release **exactly** the individually
  authenticated records and no others.
- A new mutation has no application-level success until its Flash
  completion is observed and matched to the expected identity.
- First marker durable, **second marker deliberately fails**:
  later three remain retained; reboot must preserve this outcome.
- Revoke live Gateway grant between the first and second marker:
  no further marker can start.
- Simulated power cut after one marker: the original full ACK
  **cannot** be automatically re-accepted with the formerly cached
  released item missing. A **new Gateway-signed subset ACK for the
  four remaining records** is required. After it is authenticated,
  the remaining four markers may complete safely. No loss-prone
  cumulative/high-water release, and no silent skip of unproven items.
- Verified group discarded from volatile RAM at completion/failure;
  no physical device access or old History erasure.

## Explicit limitations / unfinished product work

1. **No resumable full-group replay after partial crash.** There is no
   durable authenticated receipt progress ledger. This implementation
   is deliberately conservative: after restart, Gateway must reissue
   a freshly authenticated *subset* for unconfirmed records from its
   actual retained OCQ2 custody. This behavior is safe but may cost
   radio airtime until a tested replay scheme is approved.
2. **No implemented Gateway collection/scheduling or on-air type
   0x0A TX/RX wiring.** Existing single-custody ACK continues to work.
   The loop must reserve latest live position priority and bound
   backfill so the Tracker's five exact-cache slots do not block
   fresh locations.
3. **No actual physical role-based 28 KiB cutover.** PC 128+28 KiB
   segmented Flash tests never enabled legacy History migration on
   real RAK hardware. The no-op opt-in census stays disabled.
4. **No target build, real SoftDevice Flash async timeout, power
   measurement, key-provisioning, or multi-Gateway field trial.**
   Existing P0/not-released gate remains CLOSED.
5. Markers are **logical custody release**, not immediate erasure
   of the exact physical NOR page. Reclamation is independently
   controlled by OBS1 maintenance.

**Do not claim end-to-end offline custody finished from this PC test.
No real collar programming, no automatic History cleanup.**
