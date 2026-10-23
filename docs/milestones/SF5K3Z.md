# SF5K3Z — real host issuer signature + independent revision checkpoint

**Base:** `ea1446ee7c1923c24dd77b1eb1b718189c12235e` (SF5K3Y).

## Implemented in this candidate

- Introduces `GatewayGrantRevisionAnchor`: the authoritative **exact**
  currently enrolled signed-snapshot revision. Unlike comparing two flash
  pages, this evidence MUST be independent, authenticated, durable and
  rollback-resistant. No production implementation exists yet.
- `GatewayAnchoredGrantSource` wraps the existing issuer-verified
  `GatewayGrantSnapshotReader`. It issues no grant unless local identity
  matches AND the independent revision matches the recovered generation
  exactly. It rechecks after selecting a scoped key; on mismatch, the
  caller's key/scope buffers remain unchanged and temporary key bytes are
  wiped. It grants no authority solely from a role or visible RF header.
- Adds read-only snapshot generation + local-Gateway-ID getters needed by
  this independent gate. Nothing is installed into `main.cpp`; the normal
  `rak4630` build remains unchanged.
- Focused HOST test uses **real Ed25519 signatures**, generated during test
  execution with OpenSSL (never check in an issuer private key). It runs
  signature verification against actual OGK1 reader/OCQ2 boot guard. It
  exercises mutation-with-recomputed-CRC and wrong-issuer rejection,
  revision mismatch/revocation, and a genuine old validly signed snapshot
  after removal of the newer snapshot. This last case must fail closed.
  The host revision anchor is a controllable FAKE, not persistent hardware.
- No changes to ProductSecure/ACK wire, ProductRecordStore, FlashMutationGate
  layout, observation-record bytes, radio roles or physical flash.

## Critical outstanding security work (do not activate)

1. **RAK4630 production crypto:** Host OpenSSL proof does not compile/use a
   Nordic/Adafruit Ed25519 verifier on RAK4630. Select a supported, audited
   target cryptography API for a pinned issuer public key; confirm exactly
   the same canonical signing bytes and raw signature format, and run device
   known-answer tests. If the target prefers P-256 ECDSA, changing the proof
   algorithm is a reviewed protocol/format change. Neither a fake signature
   nor a CRC is acceptable in production.
2. **Trusted physical revision anchor:** the host mock is not a solution.
   Ordinary rewritable OGK1 flash alone is NOT a rollback-resistant anchor.
   A secure element, protected independent monotonic state or another
   carefully reviewed trusted authority is required. Revision advancement,
   reset/factory restore, dual-page recovery and lost-connectivity policy
   must be designed before accepting real grants.
3. **Provisioning and source authority:** no writer, enrollment ceremony,
   authenticated key exchange, issuer private-key custody, revocation
   publication or physical device service exists. Never hardcode TEST keys.
   Gateway can use only scoped ACK keys, not Tracker K_root. Physical access
   to unprotected flash may expose scoped keys; evaluate board protection
   and security threat model before calling at-rest keys protected.
4. **Capacity:** signed 32-slot OGK1 is a bounded preliminary format only;
   do not infer support for target fleet size.
5. **No RF qualification:** Gateway still needs approved exclusive OCQ2
   product ownership, spoofing/rate/storage exhaustion mitigation, Edge
   durable handoff and real SX126x roundtrip/relay tests.

## Validation required

```sh
bash tools/ci/run_sf5k3z_gateway_issuer_rollback_host.sh
bash firmware/tests/run_host_tests.sh
pio run -d firmware -e rak4630
```

These checks have not been run on the candidate until the operator reports
their results. The source contract and tests are non-destructive.
