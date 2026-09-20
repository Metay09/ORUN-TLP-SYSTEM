# M7P6C — CryptoCell secure-envelope primitive proof

Status: **TARGET BUILD PASS; PROBE UPLOAD PASS; HARDWARE KAT PENDING.**

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
   - flips one authentication-tag bit and requires the CryptoCell API to return
     `CRYS_AESCCM_CCM_MAC_INVALID_ERROR`.

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

## 8. Validation sequence

Before this slice can close:

1. build `rak4630_m7p6c_crypto_probe`;
2. review compile/link warnings and image size;
3. upload the probe to one owned RAK4630/RAK4631;
4. capture the single serial KAT result;
5. rebuild normal `rak4630` to prove the added test target did not disturb the
   production image;
6. run the normal host suite because `platformio.ini` is shared project
   configuration;
7. final review, then merge.

A hardware KAT PASS proves the pinned CC310 implementation against these
specific standards vectors. It does **not** prove the future ORUN secure
envelope, key lifecycle, provisioning, replay policy, RF interoperability or
battery impact.

## 9. Next gate after M7P6C

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
