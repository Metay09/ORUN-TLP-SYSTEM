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

`HistorySecureCrypto::protectNextObservation()`:

- requires a ready/provisioned/not-busy SecurityStore;
- uses the store's active DeviceIdentity, credential_id, key_epoch and K_root;
- obtains the D2A security counter only from `SecurityStore::reserveNextTxCounter()`; callers cannot supply one;
- validates caller-owned History/path/POSITION semantics before consuming a security counter;
- burns the historical SecurityStore counter value 0 once because frozen M4P4 `HISTORY_SECURE` requires a nonzero counter;
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
- failure to obtain a usable SecurityStore-owned D2A counter;
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

`test_m7p6i_history_crypto_vectors.py` independently regenerates the public
root-credential D2A key and exact first-usable-counter (`1`) nonce/AAD/
ciphertext/tag using Python `cryptography`, and locks the complete 73-byte
target-KAT frame. It is also integrated into the aggregate runner.

### Full-production-graph target KAT

`rak4630_m7p6i_history_crypto_probe`:

- boots the real production source graph;
- runs only after `Bluefruit.begin()`;
- uses the exact production `HistorySecureCrypto` class;
- provisions only a RAM-backed SecurityStore with the public M7P6E credential
  vector;
- never touches the physical SecurityStore partition;
- requires exact 73-byte D2A observation equality for the first usable SecurityStore-owned counter (`1`), independently regenerated by the M7P6I host vector gate;
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


## 12. Focused portable host validation

Owner-host focused validation on the corrected M7P6I branch:

```text
g++ -Ifirmware/include -std=c++17 -O1 -g \
  -Wall -Wextra -Werror \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  firmware/tests/m7/test_m7p6i_security_traffic_bytes.cpp \
  -o /tmp/m7p6i_bytes && /tmp/m7p6i_bytes
```

Result: **PASS**. The test is intentionally silent on success and returned to
the shell without compiler, sanitizer or assertion failure.

This closes the focused portable contract gate for:
- exact D2A/A2D direction values;
- 21-byte HKDF info serialization;
- 13-byte nonce serialization;
- epoch/counter invalid-input rejection;
- consistency with the frozen M4P4 direction constants.

Remaining before merge:
- aggregate host suite;
- fresh normal production `rak4630` build;
- physical M7P6I target KAT;
- independent focused security review.


## 13. Aggregate host regression

Owner ran the complete aggregate host suite:

```text
bash firmware/tests/run_host_tests.sh
```

Result: **PASS**.

The suite completed through all existing production-startup scenarios, M4P4
History secure vectors, BLE patch/admission/source-contract gates, R4 checks and
DEVQ1 tooling checks without failure.

The new M7P6I traffic-byte test is intentionally silent on success but is
compiled and executed under the runner's `set -e`; reaching the final DEVQ1
PASS lines therefore confirms the M7P6I gate also completed successfully.

This also confirms PR #73's restored History v4 startup host seam remains clean
after adding the production crypto source graph.

Remaining before merge:

- fresh normal production `rak4630` build;
- physical M7P6I full-graph KAT;
- independent focused security review.


## 14. Normal production RAK4630 build

Owner ran a fresh normal production build on the M7P6I branch:

```text
pio run -d firmware -e rak4630

RAM:   29,024 / 248,832 bytes (11.7%)
Flash: 265,356 / 815,104 bytes (32.6%)
SUCCESS — 65.76 s
```

Result: **PASS**.

The linked production footprint is exactly unchanged from the M4P4/SF2
baseline:

```text
RAM:   29,024 bytes
Flash: 265,356 bytes
```

This is expected because M7P6I introduces the production crypto implementation
into the source graph but still adds no normal runtime caller. Link-time garbage
collection therefore keeps the inactive seam out of the current production
image.

This is evidence that M7P6I has not yet changed normal RF, History, BLE, GNSS,
power or storage runtime behavior.

Remaining before merge:

- physical M7P6I full-graph KAT on the RAK4631 development unit;
- independent focused security review.


## 15. Physical probe upload

Owner uploaded the M7P6I full-production-graph probe image to the RAK4630/RAK4631
development unit:

```text
pio run -d firmware -e rak4630_m7p6i_history_crypto_probe -t upload

Device programmed.
SUCCESS — 34.57 s
```

The uploaded image footprint was:

```text
RAM:   37,736 / 248,832 bytes (15.2%)
Flash: 289,192 / 815,104 bytes (35.5%)
```

This establishes only successful physical programming of the intended test-only
probe image. It is **not yet** a physical crypto KAT PASS. The serial boot KAT
must still report the exact M7P6I production crypto path result.

Historical pre-hardening run note: the KAT itself used a RAM-backed public
credential, but the full production startup still executed the ordinary
physical `security_store.begin()` path before the KAT. On a blank store that
path performs no security write; nevertheless this did not prove the stronger
"physical SecurityStore untouched" claim. The post-KAT hardening in §17 makes
the probe explicitly skip physical SecurityStore startup.


## 16. Physical M7P6I History crypto KAT

The uploaded `rak4630_m7p6i_history_crypto_probe` image was exercised on the
RAK4630/RAK4631 development unit after Bluefruit/SoftDevice startup.

Serial evidence repeatedly reported:

```text
M7P6I HISTORY CRYPTO KAT PASS provision=PASS observation=PASS receipt=PASS tamper=PASS recovery=PASS
```

Result at the pre-hardening code head: **PHYSICAL PASS for the scoped M7P6I
History root-credential crypto KAT.** This evidence is retained for traceability
but is superseded as final-merge evidence by the §17 hardening changes and must
be rerun on the hardened head.

This physically demonstrates, on the pinned reference hardware/framework path,
that the actual production `HistorySecureCrypto` implementation can:

- provision the fixed public test credential into the RAM-backed SecurityStore;
- derive/protect the exact frozen SF2 DEVICE_D2A historical observation;
- authenticate/decrypt the exact frozen BACKEND_A2D two-ID durable receipt;
- reject a forged/wrong-tag receipt;
- recover immediately and successfully authenticate a later valid receipt.

The KAT executes after `Bluefruit.begin()`, so the shared CC310 lifecycle is
the production Bluefruit/SoftDevice-owned lifecycle previously qualified by
M7P6E.

Scope boundary:

- the KAT credential/state itself is RAM-backed; the pre-hardening full-graph
  startup still ran ordinary physical SecurityStore recovery as noted above;
- no production RF sender/receiver is enabled;
- no SecurityStore A2D replay state is mutated by the crypto seam;
- no History delivery watermark/checkpoint is advanced;
- no relay/gateway/backend runtime is exercised;
- this is not an RF range, outage-recovery, power, or brownout test.

Remaining merge gates after the §17 hardening:

- rerun focused/aggregate host validation;
- fresh production + probe builds;
- rerun physical KAT on the hardened probe image;
- independent focused security review of the final code head.


## 17. Post-KAT self-review hardening

Before sending M7P6I for independent final review, an internal security pass
found two issues worth correcting rather than documenting around them.

### 17.1 D2A nonce/counter ownership

The initial `protectObservation()` API accepted a caller-supplied nonzero
security counter. That was unnecessary authority leakage: a future incorrect
caller could bypass the durable nonce owner and accidentally reuse a counter
under one traffic key.

The hardened API is now:

```text
HistorySecureCrypto::protectNextObservation(...)
```

It validates all caller-owned application semantics first, then obtains the D2A
counter directly from `SecurityStore::reserveNextTxCounter()`. There is no
public History-crypto API that accepts an arbitrary D2A counter.

The existing SecurityStore reservation namespace historically begins with value
0 for a fresh credential. Frozen M4P4 `HISTORY_SECURE` rejects counter 0.
M7P6I therefore consumes/burns that one value and immediately obtains the first
usable value, **counter 1**. Burning a counter is safe; reusing one is not.

The target KAT now locks the resulting counter-1 73-byte observation frame.
A separate Python HKDF/AES-CCM vector test independently regenerates those
bytes.

### 17.2 Physical SecurityStore isolation of the probe

The first full-graph probe KAT used a RAM-backed test SecurityStore, but normal
production startup still called `security_store.begin()` on the physical
partition before the KAT. That contradicted the stronger documentation claim
that the test image never touched physical SecurityStore state.

The hardened `ORUN_M7P6I_HISTORY_CRYPTO_PROBE` startup now explicitly skips
the physical SecurityStore begin/recovery path. Its KAT uses only the separate
RAM-backed store. The unopened production SecurityStore object remains
fail-closed and its loop `poll()` is inert.

Normal `rak4630` production startup is unchanged by this test-only guard.

### 17.3 Evidence reset

Because these hardening changes alter the production crypto API and test-probe
binary after the first physical PASS, the earlier host/build/physical evidence
is historical rather than final evidence for the merge head.

Required rerun on the hardened head:

1. focused traffic-byte + independent D2A vector gates;
2. aggregate host suite;
3. normal production RAK4630 build;
4. M7P6I probe build;
5. physical probe upload + boot KAT;
6. independent focused security review.

No SF2 wire byte or TLP v1 byte changed.


## 18. Hardened-head focused revalidation

After the post-KAT security hardening, the owner reran the focused portable
contract and independent D2A crypto-vector gates:

```text
g++ -Ifirmware/include -std=c++17 -O1 -g \
  -Wall -Wextra -Werror \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  firmware/tests/m7/test_m7p6i_security_traffic_bytes.cpp \
  -o /tmp/m7p6i_bytes && /tmp/m7p6i_bytes
```

Result: **PASS**.

```text
python3 firmware/tests/m7/test_m7p6i_history_crypto_vectors.py

M7P6I SecurityStore-owned D2A vector: PASS
```

This closes the hardened-head focused validation for:

- exact traffic direction/info/nonce bytes;
- invalid epoch/counter/direction rejection;
- SecurityStore-owned first usable D2A counter = 1;
- independently regenerated HKDF-SHA256 + AES-128-CCM nonce/AAD/
  ciphertext/tag for the exact 73-byte target KAT frame.

Remaining hardened-head gates before independent review:

- aggregate host suite;
- fresh normal production RAK4630 build;
- fresh M7P6I probe build;
- physical hardened-head probe KAT.


## 19. Hardened-head aggregate regression

After the D2A counter-ownership and probe-isolation hardening, the owner reran:

```text
bash firmware/tests/run_host_tests.sh
```

Result: **PASS**.

The complete host regression reached the final DEVQ1 gates without failure.
Relevant security evidence inside the same run included:

```text
M7P6G delegated KDF/frame-key host vectors: PASS
M4P4 History secure D2A/A2D vectors: PASS
M7P6I SecurityStore-owned D2A vector: PASS
```

All production-startup scenarios also remained PASS, including the History v4
startup seam restored by PR #73.

This closes the hardened-head host regression gate.

Remaining before independent final review:

- fresh normal production `rak4630` build;
- fresh `rak4630_m7p6i_history_crypto_probe` build;
- physical hardened-head probe upload + boot KAT.


## 20. Hardened-head normal production build

After the post-KAT D2A counter-ownership and probe-isolation hardening, the
owner reran the normal production build:

```text
pio run -d firmware -e rak4630

RAM:   29,024 / 248,832 bytes (11.7%)
Flash: 265,356 / 815,104 bytes (32.6%)
SUCCESS — 16.52 s
```

Result: **PASS**.

The normal production footprint remains exactly unchanged from the prior
M4P4/M7P6I pre-hardening baseline:

```text
RAM:   29,024 bytes
Flash: 265,356 bytes
```

This confirms the hardened production crypto seam still has no active normal
runtime caller and does not alter current RF/History/GNSS/BLE/power/storage
runtime behavior.

Remaining hardened-head gates before independent final review:

- fresh `rak4630_m7p6i_history_crypto_probe` build;
- physical hardened-head probe upload + boot KAT.


## 21. Hardened-head probe build

After the D2A counter-ownership and physical-SecurityStore isolation hardening,
the owner rebuilt the dedicated full-production-graph probe:

```text
pio run -d firmware -e rak4630_m7p6i_history_crypto_probe

RAM:   37,736 / 248,832 bytes (15.2%)
Flash: 289,128 / 815,104 bytes (35.5%)
SUCCESS — 16.05 s
```

Result: **PASS**.

The hardened probe now:

- exercises the production `HistorySecureCrypto` implementation;
- obtains the D2A counter only through its RAM-backed SecurityStore;
- uses the independently locked first usable counter = 1 vector;
- explicitly skips production physical SecurityStore startup in this test-only
  environment.

Remaining hardened-head validation before independent final review:

- upload this exact hardened probe image to the RAK4630/RAK4631 development unit;
- confirm the boot KAT reports provision/observation/receipt/tamper/recovery PASS.
