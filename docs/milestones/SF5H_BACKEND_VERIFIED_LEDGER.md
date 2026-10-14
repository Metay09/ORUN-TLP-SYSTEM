# SF5H — Trusted Backend PRODUCT_SECURE Verified Ledger (candidate)

Date: 2026-10-14
Status: **REVISED HEAD HOST TESTS PENDING (conflict-quarantine fixes); NOT PRODUCTION, NOT CUSTODY ACCEPTANCE**.
Base: `main@2a936c0ab4ab59e196ccdf94e13f6045e6d5a605` (PR #128).

## Scope — real cryptography, not a mock

`backend/candidates/product_secure_verified_ledger.py` implements real
SF5B `PRODUCT_SECURE` ROOT_AEAD_V1 decryption using the maintained
Python **cryptography** library (AES-128-CCM tag length 8, HKDF-SHA256).
It uses the **exact frozen firmware contract**:

- 28-byte authenticated header, exact 84B EVENT / 100B PERIODIC,
  `DEVICE_D2A` context 0x02, profile 0x01, custody requested
  `path_flags=0x02/0x03`, exact family/size mapping and reserved 0.
- HKDF salt = 16B credential ID, IKM = 32B `K_root`;
  HKDF info = ASCII `ORUN-TLP-V2-AEAD` + direction 0x01 +
  4B big-endian key epoch; output 16B AES key.
- Nonce = 4B big-endian epoch + 0x01 + 8B big-endian security
  counter. AAD is **exact header bytes 0..27**. AEAD verification is
  mandatory **before** treating visible `device_id` as authenticated.
- Credential resolution by candidate `(device_id, epoch)` is the
  responsibility of a **trusted backend-only key provider**.
  This branch supplies only the **public SF5B test credential** in
  tests, never a real key, account or provisioning API. `K_root` or
  D2A traffic keys must NEVER be delivered to Gateway/Edge/phone.
- Post-authentication PERIODIC/EVENT schema-1 validity and canonical
  field rules are checked against the existing C++ codec constraints;
  malformed fields are rejected even if AES-CCM authenticates.

## Local SQLite verified logical ledger

The `VerifiedLocalLedger` admits only packets that pass this real
cryptographic and semantic check; uses SQLite WAL/FULL and a local
transaction plus reopened readback. The logical identity is
`(authenticated device_id, record_incarnation, record_sequence)`
(not security counter, Gateway identity, exact ciphertext hash, or
app_family). It stores canonical plaintext and the first exact
protected frame. Device ID / incarnation are BLOB(8) to preserve
the complete unsigned 64-bit namespace without SQLite signed-int
conversion problems.

- New logical identity + valid frame => `VERIFIED_STORED`.
- Same logical identity + byte-equivalent decrypted plaintext AND
  app_family (even if a different D2A counter/ciphertext) =>
  `VERIFIED_DUPLICATE` with no new row.
- Same identity + different plaintext or app_family =>
  `CONTENT_CONFLICT`, no overwrite or ACK. The **first exact authenticated
  ciphertext** for each distinct divergent family/plaintext is retained
  in a separate `verified_conflicts` forensic table with a bounded
  `max_conflicts` capacity. Replayed identical conflicts do not add rows.
  If this capacity is exhausted, the new conflict returns `UNCERTAIN`,
  never a false acceptance. The conflicting identity stays quarantined:
  **even the original otherwise-identical frame cannot subsequently be
  returned as a clean `VERIFIED_DUPLICATE`** until a future explicitly
  authorized resolution policy is implemented. This is never
  first-writer-wins. After SQLite commit, the first exact stored protected
  frame is re-opened and reauthenticated before reporting success; damaged
  local evidence yields `UNCERTAIN`.
- Bad AEAD key, tag, header, schema, unsupported flags, credential
  lookup or missing context => `REJECTED`; SQLite uncertainty =>
  `UNCERTAIN`. No record release.
- There is **no** authenticated `BACKEND_DURABLE` response, signing
  key, stable receipt/replay state, data erase or real account
  authorization. `VERIFIED_STORED` is only a local host SQLite fact
  and must not cause Edge deletion or Gateway/Tracker ACK.

The backend location is intentional: profile 0x01 holds a symmetric
authentication/decryption key that would also enable forging device
frames if disclosed. No phone/offline decryption feature is implied.

## Host tests

`firmware/tests/sf5/test_sf5h_backend_verified_ledger.py` loads the
**existing public independent SF5B vectors** directly from
`firmware/tests/sf5/sf5b_product_vectors.h`. It verifies published
PERIODIC and EVENT golden ciphertext/tag and physical RAK4631 KAT
ciphertexts against the original plaintexts. Mutation negatives
cover the header/AAD, ciphertext/tag, wrong root/credential ID,
wrong device/epoch, unsupported shapes and *authentic but
noncanonical* decrypted data.

Additional tests protect the whole logical identity namespace:
same observation across different AEAD counters/frames gives only
one SQLite row; changed valid content using the same identity
retains independent forensic evidence; cross-family collisions are
quarantined; conflict evidence survives restart; replayed conflicts
do not grow the table; known-conflicted original records never
return clean duplicate acceptance; capacity exhaustion, damaged
original ciphertext, restart and unknown/readback errors fail closed.

The integration regression also transfers actual golden product
packets over **existing local 127.0.0.1 HTTP** from Edge SQLite
to backend opaque staging SQLite, then passes the actual received
bytes into this verified backend ledger. Every staging/source row
remains retained; no ACK is emitted.

## Dependency + verification

New real cryptography requires `python3-cryptography` (the standard
Python `cryptography` package). On Debian 12, install with the
packaged system version if not already available:

```bash
python3 -c "from cryptography.hazmat.primitives.ciphers.aead import AESCCM"
# If missing: sudo apt update && sudo apt install python3-cryptography
```

The full host runner runs the new test only if the dependency can
be imported; otherwise it prints a **SKIP**, not PASS. **A full
HOST_EXIT=0 without the new explicit PASS line does NOT meet this PR's
merge gate**. No package installation is performed automatically.

Expected new line:
`SF5H backend crypto ledger: AESCCM-KAT/semantic-id/dedup/conflict/live-HTTP: PASS`.

Require the full host suite to pass on the exact PR HEAD, including
8 SF5G PASS and 4 SF5H PASS. Independent security/storage review is
required before promotion to actual backend production capability.

## Production blockers

1. Secret-management integration and per-device key epoch enrollment,
   key rotation/retirement, device/account authorization, audited
   credential provenance, and log/backup protections.
2. Transactional Backend durable receipt contract after **actual**
   durable storage; authenticated TLS + signed/object-bound application
   response or agreed equivalent, without conflating HTTP 200.
3. Real backend service routing/host authentication; current local
   loopback staging remains untrusted and must not be internet-exposed.
4. Durable source-to-ledger reconciliation and an end-to-end acceptance
   policy for delayed/old-epoch observations and deployment migrations.
5. Full external/security review, fuzzing, abuse limits, actual
   crash/power-loss measurements, database growth/performance and
   operational monitoring.

No user production data, RAK4630 flash, radio/ACK, or physical
hardware is touched by this host-only branch.
