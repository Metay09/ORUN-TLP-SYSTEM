# KQ05 — authenticated Gateway batch -> Tracker selective OBS1 release

**Status:** DRAFT. Host-only, owner's PC verification pending.
**Parent:** PR #348, SHA `fdbe40e31c15e5ea9c74506e7b8e6802364959ad`,
owner-confirmed **PC PASS** for actual SecurityStore V3 grant, 1–5 full
SHA256/HMAC and 5-object OBS1 cache. Earlier PR #346 Gateway OCQ2
durable 1–5 object readback + HMAC likewise passed the PC host gate.

## New boundary

`Kq05TrackerBatchReleaseOwner` is a loop-task owner with NO production
radio integration. It accepts ONLY the existing real
`Kq05TrackerBatchAckVerifier` outcome for authenticated v2/type 0x0A,
which matches ALL members of an ACK to uniquely retained exact
ciphertext bytes and derives the scoped Gateway key from the private
Tracker SecurityStore V3 credential.

- Verify the whole 1..5 set **before any release mutation**.
- Keep verified full 32-byte digests and the distinct logical OBS1
  record identity for each member in an opaque RAM-only capability.
- Recheck current *durable* Tracker/Gateway/epoch/floor/grant generation
  before **each** marker request; re-read the current exact ciphertext
  and confirm its 32-byte SHA256 against the stored verified digest.
- Ask the existing **private, friend-only**
  `ProductRecordStore::requestAuthenticatedCustodyRelease()` for
  **one** selected OBS1 logical identity. This is merely Flash
  admission: never count it as delivered or released.
- Let the composition root separately drive
  `ProductRecordStore::poll()`; wait for
  `takeCustodyReleaseOutcome()` and exact identity / **durable=true**
  before advancing to the next record. Yield `kReleasedOne` to the
  scheduler after each committed marker to avoid monopolizing live
  telemetry work.
- On policy revocation or mismatch, duplicate identity, damaged
  retained bytes, missing record, failed Flash marker or unexpected
  completed identity: stop the group and never infer that a later
  record was released.
- No range/HWM ACK. No whole-region erase or automatic migration.
  This code is deliberately not an ISR handler, a radio dispatch or
  a new public record deletion capability.

## Power failure/replay semantics

This group's authenticated proof is **volatile**, not a new durable
transaction log. A power cut after some committed markers leaves only
those records released in OBS1. A reboot does **NOT** reconstruct the
group or auto-release the rest. The original full ACK cannot generally
be reverified after some exact-cache records are released; a new
authenticated Gateway batch containing only the *remaining exact
records* is needed. The Gateway must still retain durable custody of
those records before it can mint a new ACK.

This rule favors safe no-data-loss semantics over automatic draining
after a partial power cut. Replays must not release unrelated records.

## PC test

`bash tools/ci/run_kq05_tracker_selective_batch_release_host.sh`

Links the **actual** SecurityStore V3, ProductRecordStore, OBS1 flash
journal, private 1–5 batch HMAC and existing OpenSSL test crypto
provider with fake NOR. Cases:

- 1..5 exact records: all-or-nothing authentication, but independent
  one-by-one durable marker completion; no premature next release.
- Partial subset: unselected cached records untouched, even if their
  logical sequence falls between selected items.
- No durable grant, invalid or truncated MAC: no marker starts.
- Failure on the **second** marker: first remains durably released,
  second and later remain untouched, both before and after reboot.
- Revoke current grant between markers: only already durable marker
  remains committed, no later release starts.
- Power cut after first marker: after cold mount neither old complete
  ACK nor local transient state can continue the remaining group;
  a separately authenticated new 4-record ACK can complete it.
- Duplicate old ACK cannot release unrelated records.

**Not tested on the owner's PC yet. No claim of physical device
evidence.** Even if this PC gate passes, this does not prove a
working Gateway-to-Tracker RF custody pipeline.

## Remaining product integration

- Gateway batch handle collection, bounded waiting time for batches
  smaller than five, RF TX/retry, and fair prioritization of new
  telemetry against historical backlog.
- Tracker type 0x0A LoRa RX routing into this loop owner; actual
  Credentials/FlashMutationGate arbitration, accurate RF duty cycle
  and eventual suppression of per-telemetry single ACKs.
- Post-reset original 5-record ACK replay will not automatically
  complete partial groups: Gateway must be able to reissue a
  surviving-subset ACK from durable custody, not rely on a sent
  packet's arrival.
- 28 KiB supplementary region role cutover on actual boards and
  NOR tear/late-SoftDevice event qualification.
- No production device upload, no physical Flash erase/History wipe,
  no claimed field release. The P0 "NOT RELEASED" gate stays in force.
