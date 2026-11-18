# KQ05 Tracker: real SecurityStore V3 verification of 1–5 Gateway ACKs

**Status:** DRAFT / PC TEST NOT YET RUN BY DEVICE OWNER.  
**Parent:** PR #347 `5795c7fb66962e5984eeba84b7678295ba868855`. The owner
verified that the actual OBS1 Flash control journal can preserve five full
exact protected objects, restart safely and refuse a sixth. The Gateway
durable OCQ2 readback→five-object HMAC host proof (PR #346) likewise passed.

## Security boundary added

Read-only `Kq05TrackerBatchAckVerifier`, distinct from the old
single-object `CustodyAckVerifier` and the Backend's type 0x09 receipt.

1. Parse only v2/type **0x0A** using the strict canonical 1–5 packet
   decoder; never mistake type 0x05/0x09 for a five-object ACK.
2. **Before accessing key material**, demand the Tracker's real
   `SecurityStore::custodyAckScopeMatches()` for the complete committed
   Tracker ID / Gateway ID / credential epoch / custody floor / grant
   generation. A raw, on-air header cannot install or select a grant.
3. Snapshot up to five full exact frames from the actual durable
   `ProductRecordStore` → `ObservationStore` control journal.
   For EACH 8-byte wire hint, require **one and only one** valid,
   currently retained, already committed 84/100-byte protected object.
   Reject a duplicate/ambiguous hint, missing item or foreign object.
4. Independently confirm each selected exact object still matches
   its retained logical OBS1 record and has no uncertain or exhausted
   Flash release marker.
5. Derive the existing `K_custody_ack` from private SecurityStore
   `credential_id` / `K_root` with the SAME 57-byte HKDF info
   contract as the previously validated 56-byte single-object ACK:
   `ORUN-TLP-V2-GW-CUSTODY-ACK-v1 || tracker || epoch || gateway || floor || generation`.
   **No public root or scoped-key accessor** is introduced.
6. Verify the independent KQ05 batch MAC domain over exact ACK
   header+sorted hints plus **all 32 bytes of each** SHA-256 digest,
   using the existing bounded CryptoCell primitive interface. Short
   prefixes alone are never authorization.
7. Wipe derivation buffers, recheck the live grant after cryptography
   and reread each exact retained object/complete digest before
   publishing an opaque, internal **authenticated (NOT released)**
   capability. On rejection the caller's output is unchanged.

This is host-only *verification*, not a device cutover, runtime radio
handler or independent release privilege. Only the planned
`Kq05TrackerBatchReleaseOwner` may later consume the private proof;
it must recheck grant, retained bytes and Flash result **before each
record's separate release-marker mutation**. A proof cannot be
submitted by an untrusted RF caller.

## Host regression

`bash tools/ci/run_kq05_tracker_batch_security_verify_host.sh`

The real SecurityStore V3 and ProductRecordStore + ObservationStore
are reused from the existing SF5K3M host fixture, with OpenSSL backing
the *same* production SHA256/HKDF/HMAC function signatures.

Checks include: no durable grant, 4/5 cached object denial, all five
authenticated, subsets of 1..5, identity-based full-digest selection
rather than cumulative range ACK, unchanged durable release markers
after authentication, duplicate/missing/tampered frames, every altered
ACK wire byte, rejection of old 0x05 and Backend 0x09 frame types,
rebooted SecurityStore/OBS1 cache, and revocation.

## Still required before usable offline custody

- No group release markers are issued by this verifier. A different
  private trusted Tracker batch release owner is needed to initiate
  five **independent**, durable and idempotent OBS1 markers, with
  failure/reboot/replay tests.
- An existing Tracker may fill five exact-cache slots and then stall
  future ciphertext caching until real ACKs arrive. It cannot be
  misrepresented as an unbounded queue. Schedule bounded ACK retries,
  protect latest live locations and make backfill opportunistic.
- Real Gateway loop/RF scheduling, durable selected-handle collection,
  LoRa 0x0A dispatch, multi-Gateway concurrency and physical Flash
  role cutover remain unqualified.
- No real device uploads, Flash erase, old-History migration or
  backend-ACK conflation. P0 field release remains BLOCKED.
