# SF5K3L — Real Tracker custody ACK SHA256/HKDF/HMAC verifier

**Status: DRAFT — new SF5K3L test has not run; RAK4630 target
compilation and physical CC310 KAT NOT YET QUALIFIED.** Based on
SF5K3K PR #148, whose **two** targeted Store/Radio tests locally PASS.

## Production functionality added

The actual `CustodyAckVerifier` now performs the complete SF5B section
12 **single-object** MAC verification. This is a real production-source
verifier; it is not a test-only model. No live composition/root caller is
enabled yet.

- Parse the exact 56-byte ACK and reject malformed fields.
- Require `SecurityStore::custodyAckScopeMatches` from the **current
  committed v3 Gateway custody authority** (Tracker/Gateway ID, epoch,
  global custody floor, exact slot grant generation). An ACK cannot
  grant or reinstate any Gateway authority.
- The `ProductRecordStore` bridge enumerates a maximum of four
  retained, durably cached exact PRODUCT_SECURE ciphertext objects via
  `snapshotCustodyCandidates`. It does NOT accept arbitrarily
  supplied nonpersistent wire buffers in production.
- For each structurally eligible retained object compute the **full**
  SHA-256. Use its first 8 bytes only as a lookup hint; refuse zero,
  repeated or ambiguous candidates even when a valid MAC exists.
- The verifier alone has `SecurityStore` friend access to current
  credential ID and K_root. It derives the 32-byte per-Gateway
  `K_custody_ack` using HKDF-SHA256 with
  `salt=credential_id[16]`, `IKM=K_root[32]`, and the exact
  57-byte info `"ORUN-TLP-V2-GW-CUSTODY-ACK-v1" || TrackerID[8]
  || epoch[4] || GatewayID[8] || floor[4] || generation[4]`.
- Authenticate the full 102-byte HMAC-SHA256 input:
  `"ORUN-TLP-V2-CUSTODY-ACK-MAC-v1" || ACK[0:40] ||
  full-SHA256-of-exact-object[32]`; compare all 16 transmitted
  HMAC tag bytes using a fixed-length constant-time comparator.
- Every secret-derived stack scratch is explicitly zeroized. Failed
  crypto calls and scope changes refuse verification; the output
  remains unchanged on failure.
- `verifyFromStore` rechecks the exact cached bytes against the
  real retained logical record after cryptographic work and only
  then publishes an opaque `AuthenticatedCustodyAck` token.
  Default-constructed token has `verified_=false`. Its identity/
  digest/grant data is private to verifier and a **future reviewed**
  release owner, not radio callbacks.
- Production algorithm provider is **pinned CC310**:
  `custody_ack_cc310.cpp` uses the existing CRYS_HASH/HKDF/HMAC
  API under the already-owned crypto lifecycle. It never calls
  `nRFCrypto.begin/end` itself.

## What this does NOT implement or prove

- No production loop caller yet takes `RadioManager::takeCustodyAckFrame`
  and passes it through this verifier. This integration and the post-auth
  owner need a separate review with same-loop flash ownership.
- No `ObservationStore::requestRelease`, no
  `ProductRecordStore` release mutator and **no custody record deletion
  is enabled**. HMAC success is authentication, not durable release.
- No actual Gateway ACK generator/signer was added. A Gateway must
  first durably commit the exact object locally or verify signed
  exact-object EDGE_DURABLE_ACCEPT, not merely receive LoRa bytes.
- A relay still must never manufacture custody evidence; reverse
  relay ACK transport has its own hop/timing/airtime/replay gate.
- This is one ACK matching one exact retained object. Batch ACK
  and backlog send scheduling are separate features.
- No RAK4630 hardware CC310 SHA/HKDF/HMAC known-answer test
  completed. Host OpenSSL and Python reference vectors are not
  a substitute for this target binary/provider confirmation.
- No production v2→v3 auto-upgrade, onboarding admin grant
  transport or security credential rotation was activated.

## Independent new SF5K3L host regression

`firmware/tests/sf5/test_sf5k3l_custody_ack_crypto.cpp` compiles the
**real** verifier and real SecurityStore with a TEST-ONLY OpenSSL
implementation of the low-level primitives. The independently
generated Python hashlib/hmac fixed oracle uses:

- device `0x0102030405060708`, Gateway `0x2300230023002301`,
  epoch 7, policy floor 0, grant generation 1;
- exact 100-byte custody-requested synthetic ciphertext-shaped frame
  with counter 42; root `90..af`, credential ID `03..12`;
- SHA256 = `92453945bd2224e1e9d462cb964b6fdc1e2018bafbe765cd4ac9ebbfc57aef00`;
- HKDF output = `b3b50abe4a260946f7a1e13361e742504736f11df4dfd3b1852e5ae242d5b04f`;
- exact 56-byte MAC-tagged ACK frozen in the host test.

Host test checks valid MAC, every mutated ACK byte, cached-object
tampering, colliding lookup hints (duplicate exact protected frame),
one-shot output/nonpublication on failure, staged grant rejection,
durable committed grant admission, durable revoke, re-enrollment
under higher generation and persisted revocation after reboot.

Short new-only qualification:

```bash
cd ~/ORUN-TLP-SYSTEM
bash tools/ci/run_sf5k3l_ack_crypto_host.sh
```

This uses the host OpenSSL development headers/library (`-lcrypto`)
and has not yet been executed. SF5K3I, J and K previous local
targeted host checks PASS; **do not repeat them** for small L changes.
The full host regression also includes L once functional fixes finish.

## Release gates

1. New L host test PASS and independent byte-vector audit.
2. RAK4630 firmware build SUCCESS, resolve CC310 API/header/link errors.
3. Physical RAK4630 CC310 SHA256/HKDF/HMAC KAT after Bluefruit
   initialization, zeroization and timing coexistence checks.
4. Gateway local custody/Edge durable accept -> ACK signer,
   authentic administrator enrollment, and end-to-end replay/cuts.
5. Privileged post-MAC `ProductRecordStore` release owner with actual
   flash readback, reboot, duplicate, wrap, and battery/airtime tests.

GitHub Actions remains blocked by account billing lock; it
never runs the job. No device flash or main merge. Keep draft.
