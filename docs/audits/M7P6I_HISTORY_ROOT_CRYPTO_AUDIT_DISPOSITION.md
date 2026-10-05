# M7P6I History root-credential crypto — independent audit disposition

Independent audit target:

```text
PR #74
baseline b0e446776b43c298e657a25777db62613a8cfb27
audited head 20757d999723e330bcdfb668b0f4da0e04e27fb2
```

Independent verdict: **PASS WITH FIXES**

Findings:

- BLOCKER: 0
- HIGH: 1
- MEDIUM: 1
- LOW: 3

## H1 — A2D authenticated credential lifetime was not returned

Status: **CLOSED — independently final-verified**

The audited seam authenticated a BACKEND_A2D receipt using the current
`credential_id` as the HKDF salt, but returned only the decoded packet and
receipt. A future SF3 caller could therefore authenticate under credential A,
defer replay admission, then incorrectly re-read current credential B and submit
A's counter under B's replay lifetime.

The fix changes `openBackendDurableReceipt()` so that, on `kOk` only, it
also returns the exact `credential_id` snapshot used as the HKDF salt for that
successful AEAD authentication.

The SF3 handoff contract is now explicitly:

```text
submitAuthenticatedA2dCounter(
    authenticated_credential_id,
    packet.key_epoch,
    packet.security_counter)
```

The caller must never re-read the current credential for a previously
authenticated receipt.

Failure atomicity is preserved: packet, receipt and authenticated credential ID
outputs remain untouched on failure.

Applied in:

- `firmware/include/history_secure_crypto.h`
- `firmware/src/history_secure_crypto.cpp`
- `firmware/tests/m7/m7p6i_history_crypto_probe.cpp`

## L1 — CC310 AES-CCM context was not explicitly wiped

Status: **CLOSED — independently final-verified**

`runAesCcm()` now zeroes `CRYS_AESCCM_UserContext_t` on every return path,
including Init failure, AAD failure, Finish/tag-size failure and success.

## M1 — broad friend access / K_root copied into crypto stack

Status: **ACCEPTED FOR THIS SLICE / FOLLOW-UP BEFORE OR WITH SF3**

The audit found no active exploit or current invariant violation. The current
friend seam is intentionally narrow in call surface, exposes no K_root getter
and zeroes temporary root/key material.

Moving HKDF ownership into SecurityStore would be a larger responsibility and
coupling change. It is not required to close H1 and is intentionally not
introduced into this already security-sensitive slice without a separate
design decision.

Re-evaluate before or with SF3 production runtime activation.

## L2 — pinned CC310 CRYS_FATAL_ERROR wrong-tag classification

Status: **ACCEPTED**

The compatibility exception remains scoped to authenticated decrypt Finish,
matching previously observed physical behavior. Both MAC_INVALID and the pinned
Finish-time FATAL path fail closed: plaintext is wiped, outputs are not written,
and replay/delivery state is not mutated.

Future diagnostics must not interpret `kAuthRejected` alone as proof of an
attacker because the pinned engine compatibility path can map there.

## L3 — additional negative-path tests

Status: **PARTIALLY IMPROVED / NON-BLOCKING**

The probe now additionally verifies that the authenticated credential lifetime
output:

- equals the public test credential on successful receipt authentication;
- remains unchanged on a wrong-tag failure;
- is correct again on immediate valid recovery.

The remaining suggested negative tests (AAD/ciphertext tamper variants,
second-observation counter progression, reboot/boundary/fault-injection cases)
remain useful before SF3 runtime activation but are not merge blockers for the
crypto-seam-only M7P6I slice.

## Required final verification

Because H1 and L1 modify production crypto code after the first independent
audit, merge still requires:

1. hardened-head probe build;
2. hardened-head physical KAT;
3. relevant host regression / normal production build as appropriate;
4. independent focused final verification of H1/L1 on the exact final head.

No TLP v1 bytes, M4P4 wire bytes, flash formats, RF runtime, History delivery
state or production secure-RF activation are changed by these fixes.


## Independent focused final verification

Final verification target:

```text
final reviewed branch head:
dea2f6df2d512c5efcd8430320049efdd3afd64c

firmware-code fix head:
ddb40700748da154907e219c425594de0c36e902
```

The verifier independently confirmed that the commits from the firmware-code
fix head through the reviewed final branch head are documentation-only.

Final verdict: **PASS**

Results:

- H1: **CLOSED**
- L1: **CLOSED**
- new BLOCKER: 0
- new HIGH: 0
- new MEDIUM: 0
- new LOW: 0

Explicit final checks:

- authenticated credential lifetime handoff: PASS
- output-on-failure atomicity: PASS
- credential snapshot zeroization: PASS
- CC310 context zeroization: PASS
- replay/delivery mutation isolation: PASS
- TLP v1 / M4P4 compatibility: PASS
- scope containment: PASS

Final merge recommendation: **MERGE**.

The verifier did not rerun physical hardware itself. Physical evidence remains
the owner-operated post-audit RAK4630/RAK4631 crypto KAT recorded in M7P6I and
is scoped only to the crypto seam. No secure-RF, store-forward, backend,
brownout or power-cut qualification is claimed.
