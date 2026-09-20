# M7P6C — CryptoCell secure-envelope primitive proof

Status: **HARDWARE KAT PASS; INDEPENDENT AUDIT FINDINGS CLOSED; SOFTWARE VALIDATION PASS; READY TO MERGE.**

Baseline: `main@1bd7e8fa0649caa1d1bbce901367ef6a81498e29`
(M6P2 merged via PR #26).

Branch: `feat/m7p6c-cryptocell-proof`.

## 1. Why this slice exists

M7P6B already provides durable per-device credential state and crash-safe TX
counter reservation, but intentionally implements no cryptography and no secure
RF envelope.

The security ADR requires the later secure-envelope milestone to validate the
actual pinned crypto implementation and published known-answer vectors before
freezing ORUN wire bytes, nonce encoding, HKDF labels or authenticated
uplink/downlink behavior.

This slice does exactly that prerequisite proof and nothing more.

## 2. Pinned implementation under test

ORUN's production target already pins:

- `nordicnrf52@11.0.0`;
- `framework-arduinoadafruitnrf52@1.10700.0 (1.7.0)`.

That framework pins `Adafruit_nRFCrypto` version 0.1.2 at submodule commit:

`2be11062ac56cf75a9a8b8ed087ad495421403f4`

and links the precompiled CryptoCell library:

`nrf_cc310_0.9.13-no-interrupts`.

The exact pinned headers expose:

- RFC5869 HKDF with SHA-256 through `CRYS_HKDF_KeyDerivFunc`;
- AES-CCM through `CRYS_AESCCM`;
- AES-128 key selection;
- CCM nonce sizes 7..13 bytes;
- CCM tag sizes including 8 bytes.

Therefore this probe does **not** add a new third-party crypto dependency and
does not implement a cipher/KDF locally.

## 3. Known-answer vectors

The test-only RAK4630 image executes two published vectors:

1. **RFC 5869 Appendix A.1**
   - HKDF-SHA256;
   - verifies the 42-byte OKM exactly.

2. **RFC 3610 Packet Vector #1**
   - AES-128-CCM;
   - 13-byte nonce;
   - 8 bytes AAD;
   - 23-byte plaintext;
   - 8-byte authentication tag;
   - verifies exact ciphertext and tag;
   - decrypts and verifies exact plaintext;
   - flips one authentication-tag bit and requires authenticated decrypt to
     return a fail-closed error;
   - accepts the dedicated `CRYS_AESCCM_CCM_MAC_INVALID_ERROR` or, for the
     currently pinned `nrf_cc310_0.9.13-no-interrupts` binary only, the
     physically observed `CRYS_FATAL_ERROR` compatibility result;
   - immediately repeats the valid decrypt and requires `CRYS_OK` plus the
     exact plaintext, proving the rejected packet does not poison later use.

The tampered decrypt output is never consumed. Authentication failure is treated
as fail-closed.

## 4. Test-only build target

New PlatformIO environment:

`rak4630_m7p6c_crypto_probe`

Source:

`firmware/tests/m7/m7p6c_crypto_probe.cpp`

This is deliberately a separate image. It does not modify the production
`rak4630` source graph or radio runtime and must not be left on a deployed
device.

Expected serial terminal result after upload:

`M7P6C CRYPTO PROBE PASS hkdf=PASS ccm_encrypt=PASS ccm_decrypt_tamper=PASS`

## 5. Security and ownership boundary

This probe uses fixed public test keys only. It does not read, expose or log the
device's M7P6B `K_root`.

The future production integration must preserve the existing rule that
`SecurityStore` has no ordinary root-key read-back API. A secure-envelope
implementation therefore needs a narrow internal secret-use boundary that can
derive/use traffic keys without creating a generic `getRootKey()` surface.

That boundary is intentionally **not** invented in this probe.

CryptoCell ownership/concurrency also remains a production design item. The
pinned Adafruit implementation uses the CC310 no-interrupt library and
`nRFCrypto.begin()` is idempotent, but this probe does not authorize arbitrary
multi-task crypto calls. Production secure-envelope work must assign a single
owner/serialization rule and review coexistence with Bluefruit/SoftDevice.

## 6. Wire/protocol impact

None.

This slice does not:

- change TLP v1 bytes or golden fixtures;
- allocate a v2 packet/application ID;
- freeze an ORUN secure-envelope header;
- freeze nonce byte layout;
- freeze HKDF salt/info labels;
- define traffic-key direction labels;
- implement authenticated ACK/contact;
- implement replay HWM;
- implement downlink;
- change relay forwarding;
- change RF parameters;
- provision credentials.

The ADR's AES-128-CCM + HKDF-SHA256 direction remains a **candidate** until this
exact target probe passes. Even after it passes, ORUN wire bytes still require a
separate reviewed specification gate.

## 7. First build finding and fix

The owner's first `rak4630_m7p6c_crypto_probe` build failed before compiling
the crypto proof itself. The synthetic target inherited production
`SX126x-Arduino` and SparkFun GNSS dependencies, which unnecessarily pulled
`Wire` into the graph. ORUN's existing pinned Wire patch includes
`Adafruit_TinyUSB.h`; in this unrelated dependency graph PlatformIO did not
propagate the TinyUSB include path to Wire, producing:

`Wire_nRF52.cpp:33:10: fatal error: Adafruit_TinyUSB.h: No such file or directory`

This is a test-target composition failure, not evidence that HKDF/AES-CCM or
CryptoCell failed. The target is now isolated from production radio/GNSS
libraries and production patch scripts, matching the existing M7P7A synthetic
build-target discipline. Production `rak4630` composition is unchanged.


Owner rerun after the isolation fix on 2026-09-20:

- `pio run -d firmware -e rak4630_m7p6c_crypto_probe`: **SUCCESS**;
- dependency graph reduced to framework-bundled Adafruit TinyUSB +
  Adafruit nRFCrypto for this synthetic target;
- RAM: **9,012 / 248,832 bytes (3.6%)**;
- Flash: **70,360 / 815,104 bytes (8.6%)**;
- no new compiler warning was visible in the supplied successful build output;
- this proves compile/link availability only. The HKDF/AES-CCM known-answer
  functions have not yet executed on hardware.


Owner upload on 2026-09-20:

- `pio run -d firmware -e rak4630_m7p6c_crypto_probe -t upload`: **SUCCESS**;
- `nrfutil` auto-detected `/dev/ttyACM0`, activated the test-only image and
  reported `Device programmed.`;
- image size remained **9,012 B RAM / 70,360 B flash**;
- the PlatformIO `99-platformio-udev.rules` message is a host setup warning;
  it did not prevent this DFU;
- upload success is not the crypto KAT result. The serial result still must be
  captured from the running probe before HKDF/AES-CCM can be called PASS.


First serial capture after upload produced no probe line while the USB CDC device
disconnected/re-enumerated around reset. This is not treated as crypto PASS or
FAIL because the original probe emitted its result only once shortly after boot;
that line could be lost before the host monitor reattached.

The test-only probe is therefore hardened to:

- wait up to 15 s for USB CDC attachment before starting diagnostics;
- print explicit BOOT / crypto-init / HKDF / CCM stage markers;
- flush each stage marker before entering the next crypto call;
- repeat the final result every 3 s while a serial monitor is attached.

This changes only observability of the test image. It does not alter the crypto
vectors, production firmware or any RF/protocol behavior.


The reconnect-safe hardware run then produced:

- crypto init: **PASS**;
- RFC5869 HKDF-SHA256 vector: **PASS**;
- RFC3610 AES-128-CCM encrypt vector: **PASS**;
- combined valid-decrypt + tampered-tag rejection check: **FAIL**.

This is a real probe failure and is not waived. It is not yet evidence of a
CryptoCell implementation defect because the combined check did not expose which
sub-condition failed. The test-only image now records the raw CC310 return code
for valid decrypt, whether recovered plaintext matches, the raw return code for
the one-bit tag tamper, and the expected CC310 MAC-invalid code. No production
code or security decision changes until that result is understood.


The detailed rerun showed:

- valid decrypt return: `0x00000000` (**CRYS_OK**);
- recovered plaintext: **MATCH**;
- one-bit tag tamper return: `0x00F50000`;
- expected dedicated CCM MAC-invalid return: `0x00F0150F`;
- therefore the data path decrypts correctly, and corrupted authentication data
  is rejected with a nonzero error, but the one-shot `CRYS_AESCCM` API in the
  pinned CC310 library reports the generic `CRYS_FATAL_ERROR`
  (`0x00F50000`) rather than the dedicated MAC-invalid code.

The probe was then switched to the explicit `CC_AESCCM_Init` +
`CRYS_AESCCM_BlockAdata` + `CRYS_AESCCM_Finish` path used by Nordic's CC310
AEAD backend. The hardware result was unchanged for the tampered tag:

- valid decrypt: `CRYS_OK`, plaintext **MATCH**;
- tampered tag: `CRYS_FATAL_ERROR (0x00F50000)`;
- immediate fresh valid decrypt after that rejection: `CRYS_OK`, plaintext
  **MATCH**.

This establishes that the pinned precompiled
`nrf_cc310_0.9.13-no-interrupts` library rejects the forged tag but collapses
that negative path to its generic fatal code rather than the header's dedicated
CCM MAC-invalid code. Because the engine immediately succeeds on a fresh valid
decrypt, the observed code is treated as a pinned-library compatibility quirk,
not as permission to accept unauthenticated plaintext.

The probe criterion is therefore narrow and fail-closed: a tampered tag passes
the negative test only if the result is either the dedicated
`CRYS_AESCCM_CCM_MAC_INVALID_ERROR` or this exact pinned-library
`CRYS_FATAL_ERROR`, and the subsequent clean decrypt must also succeed and
match exactly. Arbitrary nonzero errors are **not** accepted. Future production
code must treat every non-`CRYS_OK` authenticated decrypt as untrusted input;
it must not consume plaintext from a failed call. A future crypto-library
upgrade must rerun these vectors rather than inheriting this compatibility
exception silently.

That initial narrow hardware KAT produced PASS, but the independent review
subsequently expanded the required negative matrix and repeated-forgery stress
coverage. Therefore the earlier PASS remains valid evidence for the exact
vectors it exercised, but it is no longer sufficient by itself to close M7P6C.


Owner rebuilt and uploaded the final acceptance image on 2026-09-20:

- build/link: **SUCCESS**;
- upload: **SUCCESS**, `Device programmed.`;
- RAM: **9,024 / 248,832 bytes (3.6%)**;
- Flash: **72,016 / 815,104 bytes (8.8%)**;
- the existing PlatformIO `99-platformio-udev.rules` warning did not block
  DFU;
- final serial KAT output is still pending, so M7P6C is not yet marked PASS.


Final owner hardware KAT on 2026-09-20:

- crypto init: **PASS**;
- RFC5869 HKDF-SHA256 vector: **PASS**;
- RFC3610 AES-128-CCM encrypt vector: **PASS**;
- valid authenticated decrypt: `CRYS_OK`, plaintext **MATCH**;
- one-bit tag tamper: rejected with the pinned-library compatibility result
  `CRYS_FATAL_ERROR (0x00F50000)`;
- tamper rejection criterion: **PASS** under the narrow pinned-library rule;
- immediate post-tamper valid decrypt: `CRYS_OK`, plaintext **MATCH**;
- final serial result:
  `M7P6C CRYPTO PROBE PASS hkdf=PASS ccm_encrypt=PASS ccm_decrypt_tamper=PASS`.

This is physical evidence for the exact pinned RAK4630/RAK4631 CryptoCell
primitive path and these published vectors. It does not validate an ORUN secure
RF envelope, production key ownership, replay handling, provisioning,
BLE/CryptoCell concurrency, RF interoperability or battery behavior.

## 8. Independent audit disposition

An independent review supplied by the owner found no BLOCKER/HIGH issue, but
identified two MEDIUM test-coverage gaps that are accepted for this milestone:

1. the negative authenticated-decrypt proof mutated only one bit of tag byte 0;
2. only one forged decrypt followed by one clean recovery had been exercised.

The probe now closes both without changing production firmware:

- all 8 authentication-tag bytes are bit-flipped individually;
- ciphertext first byte and last byte are mutated separately;
- one AAD byte, the final nonce byte and one AES key byte are mutated;
- every one of those 13 well-formed mutations must return only the dedicated
  CCM MAC-invalid code or the exact pinned-library `CRYS_FATAL_ERROR`
  compatibility result;
- after every matrix rejection, a restored valid vector must immediately
  return `CRYS_OK` and exact plaintext;
- a further 1000 forged/valid alternating loop exercises repeated hostile
  input and requires every forged call to reject and every following valid
  call to recover exactly;
- successful `CRYS_AESCCM_Finish` must report the same tag size requested at
  initialization.

The review's LOW documentation/ownership findings are also accepted:

- `CRYS_FATAL_ERROR` is **not** a general authentication-failure code for
  future production logic; its compatibility treatment is limited to the
  physically observed pinned `nrf_cc310_0.9.13-no-interrupts`
  authenticated-decrypt path;
- a future production wrapper must distinguish successful authentication,
  authentication rejection and crypto-engine/argument failure rather than
  spreading the raw compatibility exception through application code;
- plaintext from any non-`CRYS_OK` authenticated decrypt must never be
  consumed and should be cleared at the production wrapper boundary;
- the isolated probe's `nRFCrypto.end()` is test-image cleanup only and must
  not be copied into production ownership because Bluefruit also initializes
  the global CryptoCell library;
- production CryptoCell serialization/coexistence with Bluefruit/SoftDevice
  remains a later secure-envelope gate. This isolated probe does not prove it.

Expanded-audit probe rebuild/upload on owner hardware (2026-09-20):

- `pio run -d firmware -e rak4630_m7p6c_crypto_probe -t upload`: **SUCCESS**;
- RAM: **9,028 / 248,832 bytes (3.6%)**;
- Flash: **75,616 / 815,104 bytes (9.3%)**;
- `nrfutil` programmed `/dev/ttyACM0` successfully;
- the existing `99-platformio-udev.rules` warning did not block DFU;
- the monitor attached after setup and therefore did not capture the one-shot
  matrix/stress detail lines, but the running expanded image repeatedly reported
  `M7P6C CRYPTO PROBE PASS hkdf=PASS ccm_encrypt=PASS ccm_decrypt_tamper=PASS`;
- in this exact expanded image, `ccm_decrypt_tamper=PASS` is gated on both
  `negative_matrix_pass` and `forged_stress_pass`. The former can become true
  only after all 13 mutation cases complete successfully; the latter only after
  all 1000 forged/valid iterations complete successfully. Therefore the final
  aggregate PASS physically closes both independent-audit MEDIUM findings even
  though the per-counter lines were not directly captured.

The independent reviewer reported the normal host suite (including
golden/compatibility tests) at rc=0 and a normal `rak4630` build at
22,084 B RAM / 225,500 B flash. Those software checks cover the shared
`platformio.ini` change. The subsequent remediation changed only the
test-only probe source and documentation, so it did not alter the production
source graph or host-test code. The expanded probe itself then rebuilt,
uploaded and passed on hardware as recorded above.

## 9. Validation result

M7P6C closeout evidence:

1. test-only CryptoCell target build/link: **PASS**;
2. test-only target upload on owned RAK4630/RAK4631: **PASS**;
3. RFC5869 HKDF-SHA256 KAT: **PASS** on hardware;
4. RFC3610 AES-128-CCM encrypt/decrypt KAT: **PASS** on hardware;
5. independent-audit 13-case authenticated-input negative matrix: **PASS**
   through the aggregate hardware gate;
6. independent-audit 1000 forged/valid recovery stress: **PASS** through the
   aggregate hardware gate;
7. normal `rak4630` production build: **PASS** as independently reported
   (22,084 B RAM / 225,500 B flash);
8. normal host suite including golden/compatibility tests: **PASS** as
   independently reported;
9. final branch diff review: no production runtime, TLP v1, RF, storage,
   identity, sequence, GNSS, power-policy or relay-forwarding change.

A hardware KAT PASS proves the pinned CC310 implementation against these
specific standards vectors and negative-path probes. It does **not** prove the
future ORUN secure envelope, key lifecycle, provisioning, replay policy,
Bluefruit/SoftDevice crypto concurrency, RF interoperability or battery impact.

## 10. Next gate after M7P6C

Only after this primitive proof passes should the project freeze the first ORUN
secure-envelope specification. That later slice must explicitly decide:

- v2 header budget / practical RF MTU;
- immutable authenticated header fields;
- origin/destination semantics;
- exact nonce construction from credential/key epoch/direction/TX counter;
- exact HKDF salt/info labels and traffic-key separation;
- relay-visible versus encrypted fields;
- mixed v1/v2 behavior;
- receive replay-state ownership;
- trusted ACK/contact semantics.

Those are protocol/security decisions, not part of this hardware crypto probe.
