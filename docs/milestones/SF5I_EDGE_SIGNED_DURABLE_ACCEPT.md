# SF5I — Signed EDGE_DURABLE_ACCEPT Host Candidate (Ed25519)

Date: 2026-10-14
Status: **DRAFT — exact new HEAD host tests pending; unapproved test-only receipt bytes**.
Base: `main@2100c2960b1ca312d71489c52e9c2d446d68cd7f`.

## Goal and owner responsibility

Actual `EDGE_DURABLE_ACCEPT` is the trusted Edge-to-Gateway custody
fact: an enrolled Edge must independently persist the **exact sealed
PRODUCT_SECURE object**, then authenticate its acceptance to the
Gateway. Once this fact is fully integrated/qualified, Gateway may
release its local copy without waiting for Backend acceptance.
**Edge must still retain its own copy until authenticated Backend
durable acceptance.** The tracker-facing
`GATEWAY_CUSTODY_ACK` is a separate downstream action with its own
credential/key and reset-safe release state.

Existing references:
- `docs/architecture/ORUN_GATEWAY_DURABLE_CUSTODY.md`
- `docs/architecture/ORUN_TLP_V2_PRODUCT_SECURE_WIRE.md`
- `docs/milestones/SF5G_EDGE_DURABLE_ACCEPT_HOST_GATE.md`

This PR **does NOT approve production receipt wire bytes**. The packet
format, key-distribution protocol and exact Gateway replay authority
still need independent security/storage review.

## Actual cryptography (not mock-auth)

`edge/candidates/edge_durable_accept_signed.py` introduces real
**Ed25519 signing** and verifying through standard `cryptography`.

- **Edge signer** owns a dedicated Ed25519 private key, distinct from
  tracker `K_root`, backend D2A key, or gateway tracker-ACK key.
- **Gateway verifier** holds only that enrolled Edge's public key,
  plus pinned expected `edge_issuer`, `gateway_instance`,
  Edge signing-key epoch and pairing grant generation.
  In contrast to shared-key HMAC, the Gateway cannot forge a
  valid Edge signature using its public verification key.
- Candidate input contains exact protected object bytes and an
  expected Gateway attempt/source tuple:
  `(gateway_instance, attempt_id, edge_issuer, source_page,
  source_slot, source_generation)`.
- Candidate receipt signs a strict version/domain-separated,
  constant-length canonical header with Edge/Gateway identities,
  attempt/source tuple, Edge key epoch, grant generation,
  exact object length and **full 32-byte SHA-256 digest** of
  the same protected frame. Ed25519 signature is 64 bytes.
  The 32-byte digest is for exact binding **under the digital signature**;
  it does not by itself authenticate or prove storage.
- `GatewayEdgeReceiptVerifier.matches()` refuses malformed length,
  wrong pinned key/issuer/Gateway/grant/key epoch, changed attempt,
  changed flash source coordinates/generation, different full frame,
  wrong signature and fallback-started cases.
- A replay of the **same exact authenticated and idempotent attempt**
  returns the same deterministic proof after a restart. A different
  attempt/source tuple or a new packet cannot reuse the old proof.
  The real Gateway still requires durable uniqueness/reservation of
  attempt identity across power cuts and actual pairing/grant
  revocation; neither is implemented here.

## Edge commit boundary

Signer checks candidate shapes without AES/decrypting product data.
It calls the existing Edge `EdgeDurableInbox.put()`, which uses
SQLite WAL/FULL, COMMIT and independently reopened exact-byte readback.

Only `COMMITTED`/`DUPLICATE` can proceed. It then invokes the
`oldest_batch()` whole-store corruption guard and checks the target
row's original ciphertext, size and digest via a second independent
SQLite readback **before** calling Ed25519.sign.

A storage `FULL`, invalid frame, simulated before/after-COMMIT
failure, readback error, corrupt backlog, different Edge enrollment,
or late arrival after local Gateway fallback => **no signature**.

This proves only the *host local SQLite execution order under the
test assumptions*. It is not actual power-cut stable-media proof.
No conditional print, HTTP 200/201, backend staging receipt,
backend ledger result, or mere socket delivery is promoted to
an Edge custody fact.

## Tests and exact boundaries

New:
`firmware/tests/sf5/test_sf5i_edge_durable_accept_signed.py`,
included in the full host suite.

Cases:
- SF5B published 84B EVENT and 100B PERIODIC golden sealed frames.
- Real ephemeral Ed25519 keypair, successful post-commit signature
  and actual verification using only the public key.
- Deterministic exact reissue after Edge DB close/reopen and duplicate
  frame; no duplicate data rows.
- Changed Gateway/attempt/Edge/source page/slot/generation, different
  ciphertext, fallback-started, signing-key epoch, grant generation,
  incorrect public key, modified signature or changed receipt bytes
  all fail closed.
- Before/after COMMIT uncertain, full storage, readback injection,
  corrupt stored exact packet and invalid pending attempts never
  produce a signature.
- `firmware/tests/run_host_tests.sh` requires `cryptography`.
  If unavailable, it prints `SF5I ... SKIP`; `HOST_EXIT=0` with
  SKIP is **not** a merge PASS.

**No production Gateway enrollment, USB/BLE transport, RAK firmware
receipt verifier, per-Gateway authorization endpoint, sidecar
service, hardware secure key storage, capability rotation/revocation,
persisted anti-replay state or safe bounded airtime test** exists in
this slice. The present method accepts caller-provided attempt
metadata: a future inbound transport must independently authenticate
and authorize the requesting Gateway and prevent enrollment/key abuse.
A compromised or malicious Edge possessing its signing private key
can lie about physical storage; trust, revocation, physical
qualification and issuer access control are deployment requirements.

This branch changes only `edge/candidates`, host tests, and a
milestone document. No RAK4630 code, flash erase, Gateway custody
ACK, tracker record release or Edge delete is enabled.

## Next integration gates

1. Independently review/freeze exact receipt, request transport,
   explicit pairing and Edge key lifecycle, authority scopes,
   reset-safe Gateway attempt/replay and Edge commit durability.
2. Implement enrolled Gateway public-key verifier in the real
   Gateway firmware, with constrained source ownership, async
   race/fallback/reboot handling and full mutation tests.
3. Implement **real host/phone connected Edge service** transporting
   only exact sealed bytes and signed receipts; restrict callers,
   protect key material and production endpoint.
4. Demonstrate physical RAK4630 Gateway, Edge real durable storage,
   drop/reconnect and power cut at **all** boundaries, Gateway
   write-around first/flash fallback second.
5. Only after these proofs should any `GATEWAY_CUSTODY_ACK` or
   flash reclaim/deletion be activated, preserving old-epoch
   downstream ownership independent from Internet.

Merge only on exact head `HOST_EXIT=0`, eight SF5G, four SF5H,
**one SF5I explicit PASS** and no SKIP/errors.
