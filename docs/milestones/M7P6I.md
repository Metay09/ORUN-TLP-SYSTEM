# M7P6I — production root-credential History crypto seam

Status: **IMPLEMENTATION IN PROGRESS — NO PRODUCTION RF/RUNTIME ACTIVATION.**

Baseline:
`main@b0e446776b43c298e657a25777db62613a8cfb27`
(PR #73 merged; M4P4/SF2 wire already frozen by PR #72).

Branch:
`feat/m7p6i-history-root-crypto`.

## 1. Why this slice exists

M4P4 froze the exact SF2 `HISTORY_SECURE` bytes, and M7P6F already owns
durable D2A TX-counter reservation plus A2D replay admission. However current
production firmware still has no root-credential HKDF/AES-CCM implementation.

M7P6E physically proved the pinned nRF52840 CC310 HKDF/AES-CCM path after
Bluefruit/SoftDevice initialization and during the scoped fresh-pairing stress,
but that implementation remained test-only.

SF3 must therefore not enable History replay until one production crypto seam
exists.

## 2. Ownership

`SecurityStore` remains the durable owner of:

- credential_id;
- key_epoch;
- K_root;
- D2A TX reservation;
- A2D replay state.

M7P6I does **not** add a K_root getter.

The only new raw-root access is a C++ friend relationship to
`HistorySecureCrypto`, whose API exposes only the two operations required by
the already-frozen SF2 History wire:

1. protect one DEVICE_D2A historical observation;
2. authenticate/decrypt one BACKEND_A2D BACKEND_DURABLE receipt.

No USB/BLE/RF/application API can read K_root through this seam.

## 3. Crypto contract

M7P6I consumes, unchanged, the independently reviewed M7P6D/M7P6E root traffic
contract:

```text
K_traffic =
HKDF-Expand(
  HKDF-Extract(SHA-256, credential_id, K_root),
  ASCII("ORUN-TLP-V2-AEAD") || direction_u8 || key_epoch_be32,
  16)

nonce =
key_epoch_be32 || direction_u8 || security_counter_be64

direction:
0x01 = D2A
0x02 = A2D
```

AES-128-CCM uses a 13-byte nonce and 8-byte tag.

M4P4 header context values remain a separate namespace:

```text
0x02 DEVICE_D2A  -> crypto direction 0x01 D2A
0x01 BACKEND_A2D -> crypto direction 0x02 A2D
```

The portable `security_traffic_bytes.h` helper freezes only these
already-reviewed info/nonce bytes. It is not a general crypto framework.

## 4. Production History crypto seam

`HistorySecureCrypto::protectObservation()`:

- requires a ready/provisioned/not-busy SecurityStore;
- uses the store's active DeviceIdentity, credential_id, key_epoch and K_root;
- accepts a nonzero caller-owned D2A security counter;
- serializes the exact M4P4 historical POSITION plaintext;
- builds the exact frozen 36-byte History AAD through the existing codec;
- derives the D2A traffic key;
- AES-CCM encrypts the 29-byte observation;
- returns a complete `HistorySecurePacket`;
- performs no RF send and no History mutation.

`HistorySecureCrypto::openBackendDurableReceipt()`:

- structurally decodes the exact frozen M4P4 envelope;
- requires BACKEND_A2D/BACKEND_DURABLE family;
- requires target DeviceIdentity == local device;
- requires current key_epoch;
- requires the caller-supplied current History incarnation;
- derives the A2D traffic key;
- authenticates/decrypts the receipt;
- validates the exact explicit-ID receipt plaintext;
- leaves output objects untouched on failure;
- performs **no** SecurityStore replay mutation and **no** History delivery
  mutation.

The future SF3 owner must call
`SecurityStore::submitAuthenticatedA2dCounter()` only after crypto returns
`kOk`, and may apply receipt identities only after replay admission returns
accepted.

## 5. Fail-closed behavior

The seam rejects/fails closed for:

- unprovisioned/fault/foreign/unsupported/busy SecurityStore;
- zero security counter;
- invalid/future key epoch;
- wrong device binding;
- wrong History incarnation;
- wrong context/family;
- malformed frozen codec/plaintext;
- HKDF/CC310 engine failure;
- AES-CCM authentication failure.

Failed authenticated decrypt plaintext is never dispatched.

The pinned CC310 compatibility exception remains narrow: M7P6C/M7P6E physically
observed `CRYS_FATAL_ERROR` on the wrong-tag CCM Finish/decrypt path.
M7P6I classifies that code as `kAuthRejected` only when it came from the
authenticated decrypt **Finish** stage. A FATAL from init/AAD/other operations
remains an engine failure.

Temporary root/key material is explicitly zeroed after use.

## 6. Bluefruit / CC310 lifecycle

Production `HistorySecureCrypto` does not call:

- `nRFCrypto.begin()`;
- `nRFCrypto.end()`.

M7P6E already established that Bluefruit owns the shared lifecycle.

M7P6I itself adds no normal production caller, so normal RF/power scheduling is
unchanged. Before SF3 enables this path, scheduling must retain the M7P6E
physical timing finding that one KAT reached ~98.6 ms during fresh pairing; do
not assume crypto always finishes near the ~2 ms idle observation.

## 7. Tests

### Portable host gate

`test_m7p6i_security_traffic_bytes.cpp` locks:

- D2A/A2D direction values;
- exact 21-byte HKDF info for epoch `0x01020304`;
- exact 13-byte D2A/A2D nonce bytes;
- invalid direction;
- `UINT32_MAX` epoch rejection;
- zero counter rejection;
- consistency with frozen M4P4 direction constants.

It is integrated into the aggregate host runner.

### Full-production-graph target KAT

`rak4630_m7p6i_history_crypto_probe`:

- boots the real production source graph;
- runs only after `Bluefruit.begin()`;
- uses the exact production `HistorySecureCrypto` class;
- provisions only a RAM-backed SecurityStore with the public M7P6E credential
  vector;
- never touches the physical SecurityStore partition;
- requires exact M4P4 73-byte D2A observation frame equality;
- decrypts the exact frozen A2D two-ID receipt;
- flips the receipt tag and requires authentication rejection;
- requires a valid decrypt immediately after the rejected forgery;
- repeats the PASS line so USB monitor attach timing cannot hide the result.

## 8. Hard non-goals

M7P6I does not:

- transmit or receive secure History over LoRa;
- change RadioManager or the 10-second TRACKER RX window;
- select/retry backlog records;
- call SecurityStore A2D replay admission in production;
- advance History delivery state;
- authorize HISTORY_SECURE relay forwarding;
- add gateway/backend runtime;
- add provisioning;
- change TLP v1 bytes;
- change any flash format;
- change GNSS/BLE/power policy.

## 9. Validation required before merge

1. focused portable M7P6I host test with warnings-as-errors + ASan/UBSan;
2. aggregate host suite;
3. normal production `rak4630` build;
4. full-graph `rak4630_m7p6i_history_crypto_probe` build;
5. physical probe boot KAT on the RAK4631 development unit;
6. independent focused security review of:
   - K_root ownership/non-export;
   - exact KDF/nonce bytes;
   - M4P4 AAD binding;
   - wrong-tag classification scope;
   - output-on-failure behavior;
   - no premature replay/delivery mutation.

No SF3 RF runtime is authorized by this milestone alone.

## 10. Next step

After M7P6I closes, M4P5/SF3 can bind:

```text
History oldest-undelivered selection
 -> SecurityStore durable D2A counter
 -> HistorySecureCrypto protection
 -> bounded RF scheduling
 -> authenticated BACKEND_DURABLE receive
 -> SecurityStore A2D replay admission
 -> bounded RAM explicit-ID receipt set
 -> contiguous History watermark/checkpoint
```

That next slice must still preserve live/critical priority, collision-domain
admission, retry/backoff and the M4P4 airtime limits.


## 11. Full-graph probe build

After correcting one compile-only AES-CCM call-site argument mismatch in
`HistorySecureCrypto::protectObservation()`, the dedicated target build passed:

```text
pio run -d firmware -e rak4630_m7p6i_history_crypto_probe

RAM:   37,736 / 248,832 bytes (15.2%)
Flash: 289,192 / 815,104 bytes (35.5%)
SUCCESS — 66.43 s
```

Result: **PASS**.

The size above belongs to the test-only full-production-graph probe image, which
adds the RAM-backed SecurityStore KAT state and probe code. It is not a normal
production footprint claim.

The compile failure that preceded this PASS was limited to an incorrect extra
argument in the AES-CCM encrypt helper call; no wire/security semantics changed
when it was corrected.

Still required before merge:

- portable focused M7P6I host test evidence;
- aggregate host suite;
- fresh normal production `rak4630` build on the corrected head;
- physical probe boot KAT;
- independent focused security review.
