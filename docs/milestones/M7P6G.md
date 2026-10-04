# M7P6G — Delegated gateway KDF / frame-key host vectors

Status: **IMPLEMENTATION CANDIDATE — HOST-ONLY; OWNER VALIDATION PENDING.**

Baseline:
`main@8128b5113f7f924610a023e1eff097f98e46addb`.

Branch:
`test/m7p6g-delegated-kdf-vectors`.

## 1. Purpose

This is the first still-missing implementation gate in the owner-approved
delegated gateway-command sequence.

Earlier M7P6E evidence proves the generic candidate credential PRK,
D2A/A2D traffic-key, nonce and pinned CC310/Bluefruit coexistence path.
M7P6F separately proves durable backend-A2D replay persistence.

Neither slice proves the delegated gateway-specific hierarchy:

```text
credential PRK
  -> K_grant("ORUN-TLP-V2-GW-GRANT-v1", gateway/floor/generation/scope/quota)
  -> K_frame("ORUN-TLP-V2-GW-FRAME-v1", delegated direction, frame salt)
```

M7P6G adds only deterministic public host vectors for that candidate hierarchy.
It does not activate production crypto or allocate final command bytes.

## 2. Candidate vector

Public fixture inputs:

```text
K_root:                    00..1f
credential_id:             a0..af
key_epoch:                 0x01020304
gateway_device_id:         0x1122334455667788
gateway_policy_floor:      0x0a0b0c0d
gateway_grant_generation:  0x10203040
scope_id:                  0x03
quota_code:                0x02
frame_key_salt:            d0..db
security_counter:          0x0102030405060708
GW2D context:              0x03
D2GW context:              0x04
```

Expected outputs:

```text
PRK:
e923d7ce41cdafb9ff36e7d38e640888600785351ef83c5adb8ea0c403881a5d

K_grant:
d5a8504044d58a88cec23d0a62a1bababc999e010d081a57ded5a614e06a9383

GW2D K_frame:
40f3bd31f3bdf590c0ab7ca5fcd25594

D2GW K_frame:
e9709b691f0f5fb362db538212e78b64

GW2D nonce:
01020304030102030405060708

D2GW nonce:
01020304040102030405060708
```

The host test independently recomputes RFC5869 HKDF-Extract/HKDF-Expand with
Python standard-library HMAC-SHA256 and compares against the shared fixture.

## 3. Binding checks

The host test also proves that changing any one of:

- key epoch;
- gateway device ID;
- policy floor;
- grant generation;
- scope;
- quota;

changes `K_grant`.

It separately proves that delegated direction and `frame_key_salt` both change
`K_frame`.

These are deterministic KAT/binding checks, not a cryptographic proof.

## 4. Hard boundary

This slice changes only:

- test-only vector material;
- host-test integration;
- this milestone evidence record.

It does **not** change:

- `firmware/src/`;
- `firmware/include/`;
- TLP v1;
- production radio behavior;
- tracker RX window/power behavior;
- SecurityStore or ConfigStore;
- BLE;
- final `DELEGATED_SECURE_APP` wire bytes;
- COMMAND/RESULT plaintext;
- gateway persistence;
- production AES-CCM/HKDF execution.

No physical test is required for this host-only slice.

## 5. Next gate

After host validation and focused review, the next delegated slice is the exact
`DELEGATED_SECURE_APP` codec golden/malformed contract. A later target slice
must run these same delegated KDF/frame-key bytes on the pinned RAK4630 crypto
path and include the required CSPRNG/coexistence evidence before production
secure receive is enabled.
