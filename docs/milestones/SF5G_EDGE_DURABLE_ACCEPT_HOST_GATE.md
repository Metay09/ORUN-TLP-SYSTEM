# SF5G — Edge durable acceptance: host-only binding/replay gate

Date: 2026-10-14
Status: **HOST CANDIDATE / NEW HEAD NOT YET TESTED / NOT WIRE OR PRODUCTION**.
Base: `main@36da5d7c7faa7754fc5d138448ab9b7bf81b577a` (PR #124).

## Existing authoritative contracts

* `docs/architecture/ORUN_GATEWAY_DURABLE_CUSTODY.md` distinguishes
  **Gateway custody**, **Edge durable custody**, **backend durability**
  and application delivery. A network response, HTTP 200, socket
  completion, BLE notification, RF receipt or CRC is not custody.
* `docs/architecture/ORUN_TLP_V2_PRODUCT_SECURE_WIRE.md` requires
  an authenticated **exact-object** `EDGE_DURABLE_ACCEPT` before the
  connected write-around path can release Tracker responsibility
  without Gateway flash. A late response after local flash fallback
  starts MUST NOT cancel or rewrite that mutation. Tracker-facing
  `GATEWAY_CUSTODY_ACK` is a distinct, later authenticated mechanism.
* The repository has **not frozen any actual Edge accept frame bytes,
  credential/issuer trust model or production Edge cryptographic
  verifier**. This candidate MUST NOT invent an on-wire protocol or
  silently reuse HISTORY_SECURE BACKEND_DURABLE semantics/keys.

## Narrow host-only implementation

`sf5g_edge_accept_gate_candidate.h/.cpp` adds an intentionally
non-privileged `evaluate()` seam with these exact assumptions:

1. The Gateway has one pending 84/100B `PRODUCT_SECURE` exact sealed
   frame, valid source page/slot/generation, nonzero instance/attempt
   identifiers and an explicitly selected expected Edge issuer.
2. The future `ReceiptVerifier` authenticates the **whole** receipt
   and returns a post-authenticated claim *bound to* issuer, gateway,
   attempt, source physical page/slot/generation and exact
   byte-for-byte sealed frame, with a valid epoch/counter and Edge
   durability scope. The current test-only verifier is a mock.
3. `evaluate()` rejects absent/invalid inputs, local fallback
   already started, verifier rejection/unavailability, wrong issuer,
   stale attempt, wrong page/slot/generation, size/bytes differences,
   transport-only responses and invalid epoch/counter BEFORE touching
   any replay state.
4. For a fully matching mocked claim, the separate `ReplayStore`
   must return `kDurablyAccepted`: this is a **contract assertion**
   that replay state was persisted/read back and survives reboot.
   Busy, read-failure, uncertain or duplicate/old result fails closed.
   The host mock models the persisted monotonic counter and rebuilds
   its adapter after simulated restart.
5. Only in that model does the result become **`kCandidateOnly`**.
   It is *not* an authentic receipt, ownership transfer, flash erase
   capability or permission to emit `GATEWAY_CUSTODY_ACK`.

No receiver, real MAC/AEAD, root credential, privileged proof
constructor, authenticated channel, actual persistence, replay epoch
rotation, source flash validation or real Edge durability is
implemented. The public `VerifiedClaim` is **forgeable in host
tests**; it MUST NOT be connected directly to a production action.
Likewise no independent replay source is activated. Real Edge keys
and exact wire encoding/nonce/replay rules need a separate reviewed
contract. The existing security store may inform the design, but
cannot automatically be repurposed because its A2D History semantics
and issuer identity differ.

The host-only classes are in `firmware/tests/sf5/` and linked by
`firmware/tests/run_host_tests.sh` only. They do not alter
`firmware/src`, `firmware/include`, hardware flash, Gateway runtime,
radio or ACK.

## Host regression matrix

* 84B EVENT and 100B PERIODIC mock claims, mock durable replay
  acceptance, duplicate/old counters rejected after constructing a
  new replay adapter over persistent test state.
* A new attempt with larger counter accepted; reusing an old counter
  rejected.
* Fake raw reply, explicit crypto rejection, unavailable verifier:
  no replay consumption.
* Replay unavailable or duplicate: no positive result and no
  mock-persistent counter advancement.
* Mutate **each** authenticated identity dimension: issuer, gateway,
  attempt, source page/slot/generation, exact bytes, length, epoch,
  counter and transport-only scope => reject before replay mutation.
* Invalid physical source/count, missing object custody request,
  bad object length, zero identities, null receipt and empty receipt.
* Late verified Edge claim after local fallback started => refuse
  without consulting verifier/replay store.

## Future gates — MUST precede actual ACK/reclaim/activation

1. Define the exact `EDGE_DURABLE_ACCEPT` authenticated wire/channel,
   cryptographic issuer trust, credential rotation, epochs, nonce and
   persistent replay reservation semantics. Review the security
   boundary independently at this **storage-critical integration
   milestone**, rather than auditing each host-only commit.
2. Implement and test a real Edge durable ingestion endpoint with
   persisted exact-frame idempotency. Ensure its signed/authenticated
   durable accept is issued **only after data is safely on stable
   storage** and is bound to the original exact protected bytes.
3. Integrate *real* Gateway verification + durable replay ownership,
   bounded async storage and reset recovery. Preserve the race rule:
   once local fallback starts, late Edge accept cannot cancel it.
4. Make tracker custody release distinct from Gateway→Edge receipt,
   and never send tracker ACK before confirmed responsibility.
5. Build actual reclaim eligibility/intent/erase state machine with
   crash-safe copy-forward and no loss of HELD records, full-region
   ownership, physical power-cut testing and closed SF4B N1–N5.

## Test gate

The new exact feature-branch HEAD must pass the full host suite with
ASan/UBSan and warnings-as-errors. Expect **eight** SF5G PASS lines
including:
`SF5G Edge accept gate: mock-auth/exact-binding/durable-replay/fallback: PASS`.

No runtime or physical flash qualification is claimed from a host PASS.
