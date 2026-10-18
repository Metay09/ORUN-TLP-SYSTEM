# SF5K3M — Real post-HMAC Tracker custody release owner

**Status: DRAFT; new SF5K3M host test, full regression, target RAK4630
build and physical flash/CC310 KAT have NOT RUN.** Based on locally
PASS-confirmed SF5K3L crypto/HMAC verifier (PR #149).

## Scope

SF5K3M adds a **new privileged owner** around the existing real
ProductRecordStore -> ObservationStore flash release path.
No new record format, new flash partition, new ACK wire bytes or radio
TX schedule is introduced.

- `CustodyAckReleaseOwner::submitAuthenticated` accepts ONLY an
  opaque `AuthenticatedCustodyAck` minted after successful full
  SHA256/HKDF/HMAC verification. A default token fails closed.
- The token now contains the authenticated Tracker ID and credential
  epoch as well as Gateway ID, policy floor, grant generation, full
  32-byte exact-object digest and logical record identity.
- At release admission, recheck that the **same committed Gateway grant**
  remains currently authorized in the real v3 SecurityStore. A staged,
  unread, revoked, re-enrolled, wrong epoch/floor/generation Gateway is
  not release authority.
- Re-read the exact ciphertext from the real ProductRecordStore control
  journal, require byte-identical live retention, recompute all 32 bytes
  SHA256 and compare constant time against the private authenticated
  digest. Recheck grant and retention again immediately before mutation.
- `ProductRecordStore` owns the **only private custody release mutation**
  API (friend-only to `CustodyAckReleaseOwner`). It calls the existing
  `ObservationStore::requestRelease(identity)`, exclusively consumes
  `takeReleaseResult` during its ordinary `poll()`, and requires a
  **committed, readback verified released record + absence from the live
  exact-object lookup** before reporting `kDurablyReleased`.
- Flash `kPending` is NEVER considered durable; flash error/unknown
  result is never emitted as a positive release. No data is erased from
  the product region as an ACK side effect. The existing two-slot
  irreversible release marker remains authoritative.
- Replayed ACKs cannot release an already-released record again because
  the exact cache is no longer eligible; the old ciphertext is not
  counted among the four active outstanding custody objects.
- No second release request or cache/period/preparation request may
  steal the flash mutation/result owner while a custody release is
  pending or has unread completion.

**Security scope**: only HMAC authentication can mint a token; any
owner holding an authenticated token MUST still revalidate the current
policy and exact stored object at release admission. This does not
prove that a Gateway truly committed the record: enrolling a malicious
custody Gateway creates the documented trust-anchor risk.

## New actual Store host test (not yet executed)

`bash tools/ci/run_sf5k3m_release_host.sh`

The new test links the REAL `SecurityStore`, `CustodyAckVerifier`,
`CustodyAckReleaseOwner`, `ProductRecordStore` and
`ObservationStore` with fake NOR/async completion and TEST-ONLY
OpenSSL crypto primitive port. It uses the previously frozen
independent SF5K3L signed ACK oracle (synthetic ciphertext-shaped
test object, NOT backend-verified AEAD).

Coverage:
1. Valid HMAC exact cache -> private release request -> flash completion
   -> readback -> durable release, synchronous and pending flash.
2. Reject default/forged-empty token and competing releases.
3. No active custody candidate after release, and old ACK cannot be
   authenticated again on the released cache.
4. Durable release survives reboot on same unmodified NOR bytes.
5. Revocation or re-enrollment between HMAC and release admission
   rejects the stale proof; retained observation remains intact.
6. Physical release marker program failure before mutation cannot claim
   durable success; reboot retains the original observation.

Full host runner and focused host runner also invoke the M test. K/L
focused tests have already been locally verified and do not need
repeating for the initial SF5K3M iteration.

## Critical product activation blockers

1. Source cannot yet claim success until SF5K3M host test PASS, full
   host regression and `pio run -d firmware -e rak4630` succeed.
2. Real RAK4630 CC310 SHA/HKDF/HMAC physical KAT and BLE/flash lifecycle
   review are still mandatory.
3. **Production composition has NO live caller** to consume the real
   `RadioManager::takeCustodyAckFrame`, invoke the HMAC verifier and
   submit an authenticated token to this release owner. This is a
   separate runtime/ownership integration gate.
4. Gateway-side **durable exact-object intake + signed 0x05 ACK emission**
   and authenticated Gateway custody enrollment/admin transport remain
   separate. No Gateway may ACK based on RX/TX_DONE alone.
5. Reverse Relay ACK forwarding, lost-ACK timing, node sleep, airtime/
   duty cycle, hardware reboots/cuts and full multi-node field tests
   remain necessary.

**No automatic product cutover or actual flash wipe is enabled.**
PR remains DRAFT and GitHub Actions remains blocked by the confirmed
billing lock, independent of local host test results.
