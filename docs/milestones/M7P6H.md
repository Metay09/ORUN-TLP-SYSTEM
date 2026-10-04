# M7P6H — DELEGATED_SECURE_APP exact envelope codec

Status: **IMPLEMENTATION CANDIDATE — FOCUSED HOST VALIDATION PENDING.**

Baseline:
`main@d25d51d597735cc6611820e60ac6084716a0090e` (M7P6G merged).

Branch:
`feat/m7p6h-delegated-secure-codec`.

## 1. Purpose

M7P6G closed the delegated gateway KDF/frame-key host-vector gate.

M7P6H turns the already owner-approved delegated 56-byte header direction into
one exact bounded envelope codec before any production crypto or RF receive path
is enabled.

This slice freezes only the delegated envelope/container bytes:

- protocol version `0x02`;
- packet type `0x01` = `DELEGATED_SECURE_APP`;
- security contexts `0x03` = `DELEGATED_GW2D`,
  `0x04` = `DELEGATED_D2GW`;
- application family `0x01` = `COMMAND`,
  `0x02` = `RESULT`;
- 56-byte authenticated header;
- 0..32-byte ciphertext container;
- 8-byte AES-CCM tag;
- exact big-endian integer offsets and reserved-bit rules.

The complete COMMAND/RESULT plaintext layouts remain separate later contracts.

## 2. Exact envelope

All integers are big-endian.

```text
off  size  field
0    1     version = 0x02
1    1     type = 0x01
2    1     security_context
3    1     app_family
4    1     path_flags
5    1     grant_flags
6    1     ciphertext_len
7    1     reserved = 0

8    8     origin_device_id
16   8     target_device_id
24   4     key_epoch
28   8     security_counter
36   4     gateway_policy_floor
40   4     gateway_grant_generation
44   12    frame_key_salt

56   N     ciphertext
56+N 8     authentication tag
```

AAD remains exactly bytes 0..55.

Total packet size is exactly `64 + ciphertext_len`, bounded to 64..96 bytes.

## 3. Initial context/family matrix

```text
0x03 DELEGATED_GW2D  -> 0x01 COMMAND
0x04 DELEGATED_D2GW  -> 0x02 RESULT
```

Every other combination rejects before crypto/application dispatch.

This numeric family allocation is frozen by this envelope slice. It does not
freeze COMMAND opcode values, RESULT codes, scope registry values or plaintext
field layouts.

## 4. Validation rules

The codec rejects:

- wrong version or packet type;
- unknown delegated security context;
- unknown application family;
- context/family mismatch;
- unknown `path_flags` bits;
- reserved `grant_flags` bits;
- ciphertext length above 32;
- non-zero reserved byte;
- physical length different from `64 + ciphertext_len`;
- `key_epoch == 0xFFFFFFFF`;
- `security_counter == 0`;
- `gateway_policy_floor == 0xFFFFFFFF`;
- grant generation 0 or `0xFFFFFFFF`.

The codec deliberately does not guess final COMMAND/RESULT plaintext minimums.
It accepts a 0..32-byte ciphertext container. Exact family plaintext decoders
must enforce their own minimums when those contracts are separately frozen.

## 5. Golden/malformed coverage

The focused host test locks:

- exact 96-byte maximum-size golden frame;
- serialize -> golden byte equality;
- golden -> deserialize round trip;
- 64-byte zero-ciphertext envelope boundary;
- all validation failures listed above;
- both allowed context/family directions;
- serializer-side semantic rejection.

The test is built with the repository's existing portable
warnings-as-errors + ASan/UBSan host flags.

## 6. Hard boundary

This slice adds a reusable codec source/header but does **not** reference it from
the production composition.

It does not change:

- current TLP v1 bytes;
- radio RX/TX scheduling;
- TRACKER 10-second receive policy;
- AES-CCM/HKDF execution;
- SecurityStore replay mutation;
- gateway authority persistence;
- relay custody;
- COMMAND/RESULT plaintext;
- BLE/GNSS/geofence/history/power behavior.

No RAK build or physical-device test is required while the new codec remains
unreferenced by production composition.

## 7. Next gate

After focused host validation and review, the next security slice should run the
delegated M7P6G KDF/frame-key contract against the pinned target crypto path and
then build the secure receive path only after the required delegated replay
persistence/authority-state prerequisites are reconciled.
