# KQ05 Gateway selective batch custody ACK — real SHA256/HMAC host slice

**Status:** DRAFT, PC verification pending, NOT radio- or device-active.  
**Parent:** PR #344, `c13b3c8fce2ab3cff0d80a573a195fefe9004438`,
which the owner confirmed **PC PASS** for the structural parser.

## Added security boundary

The private `Kq05GatewayBatchCustodyCrypto` core uses the **real existing**
`custody_crypto::sha256` and `custody_crypto::hmacSha256Bounded`
provider contracts. Its host executable links the independent existing
OpenSSL test provider. Production will use CryptoCell/CC310 only after
a separate target-build qualification.

For 1–5 exact retained objects within ONE Tracker/Gateway immutable
scope, the helper:
1. Validates each PRODUCT_SECURE frame with the actual v2 frame decoder,
   including Tracker ID, epoch and custody-request bit. It does **NOT**
   authenticate an encrypted object's AEAD origin.
2. Computes the **full 32-byte SHA256** of every exact 84/100-byte
   frame, sorts them by strictly increasing 8-byte digest prefix and
   rejects any duplicate/ambiguous prefix or repeated frame.
3. Constructs v2/type `0x0A` with scoped Tracker/Gateway IDs, credential
   epoch, custody floor and grant generation. A short prefix is an
   index only, NEVER a sufficient authorization.
4. Computes `HMAC-SHA256(K_custody_ack,` the independent
   `ORUN-KQ05-GW-BATCH-ACK-V1` domain, exact header+8N hints and
   concatenated **full** SHA256 values in the same canonical order`)`;
   publishes only the first 16 bytes. The max HMAC input fits the
   existing bounded 272-byte provider.
5. The read-only verifier recomputes full hashes from the provided
   retained objects, checks the EXACT trusted scope and constant-time
   MAC. Any changed, duplicate, foreign, unretained or missing object
   denies the entire candidate without record mutation.
6. Caller output and output-length are preserved on sign failure;
   signing material and intermediate hashes are zeroed after use.

The entrypoints are **private** and accessible only to future trusted
`Kq05GatewayDurableBatchOwner` /
`Kq05TrackerBatchReleaseOwner` classes, plus an explicitly
`ORUN_KQ05_BATCH_HMAC_HOST_TEST`-gated test peer. They must not be
exposed to SX126x RX callbacks or generic ingress.

### Host verification

`bash tools/ci/run_kq05_gateway_batch_ack_hmac_host.sh`

This host test checks widths 1..5, canonical reordering, each exact
frame mutation, spoofed grant/epoch/Gateway, bad HMAC, missing frame,
duplicate object, no-custody frame, invalid shape, and failure output
immutability. It freezes an independent Python `hashlib`/`hmac`
five-frame 88-byte oracle, not merely a C++ sign-then-verify loop.

**Not yet executed on the owner's PC**. This PR adds no firmware radio
packet handler, LoRa TX, History flash migration or real storage release.

### Unfinished mandatory product gates

- **Trusted Gateway source of N durable objects:** ProductCustodyStore
  must commit, read back and retain each exact object before signing;
  its current ingress is single-pending, not a full five-record batch
  collector. No public crypto API itself proves physical custody.
- **Trusted Tracker retained exact objects:** the existing
  ProductRecordStore exposes at most four cached exact objects,
  so 5-object matching needs another bounded strategy without
  turning the 4-cache bottleneck into a permanent constraint.
- **SecurityStore grant derivation/revocation:** read-only HMAC verifier
  currently receives a *test-supplied* scope and scoped key, not a
  locally durable and authorized grant. The future privileged owner
  must access the real SecurityStore and recheck current grant after
  cryptographic work and before each release.
- **Selective durable release + replay/partial failure:** one MAC
  does not release the group. Each exact currently retained sequence
  must receive a separately verified Flash release marker; a missing
  or failed marker must not erase any other observation.
- **2+ Gateway / offline LoRa:** never confuse Gateway custody with
  Backend durable receipts, never create an ACK on a RAM-only event;
  link-budgets, priority/fairness, power-cut behavior and hardware
  qualification remain to be tested.

P0 field release stays **BLOCKED**.
