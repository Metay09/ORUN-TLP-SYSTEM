# M7P6E — CryptoCell + Bluefruit/SoftDevice coexistence proof

Status: **SOFTWARE/HOST/BUILD VALIDATION PASS; OWNER HARDWARE COEXISTENCE VALIDATION PENDING.**

Baseline: `main@00a96811c73cd5f9609f6869eb2c4a055013842b`
(PR #29 merged after M7P6D).

Branch: `feat/m7p6e-cryptocell-bluefruit-coexistence`.

## 1. Purpose

M7P6C proved the pinned CryptoCell primitives only in an isolated target image.
M7P6D then recorded the independently reviewed candidate security contract but
left one production blocker explicit: Bluefruit already initializes and uses the
same global `nRFCrypto` / CC310 facility, including LESC pairing work from a
different FreeRTOS task.

M7P6E is the smallest code-bearing slice that answers:

> Can the exact candidate ORUN HKDF/AES-CCM operation run repeatedly in the
> real production source graph while SoftDevice/Bluefruit is active, including
> an observed LESC pairing event, without crypto failure or BLE disconnect?

This is still a **test-only proof**. It does not put cryptography into the
production LoRa packet path.

## 2. Scope and non-goals

M7P6E adds:

- one shared public candidate-vector fixture used by host and target tests;
- one pure host byte-domain check for KDF-info and nonce serialization;
- one target-only CC310 KAT helper;
- one full-production-graph PlatformIO probe environment;
- test-only serial commands and BLE security-event counters behind
  `ORUN_M7P6E_CRYPTO_BLE_PROBE`;
- boot-time KAT after `Bluefruit.begin()`;
- bounded connected stress that requires a LESC DH-key event to occur while the
  repeated ORUN KAT is active.

It does **not**:

- read or export `SecurityStore::K_root`;
- provision credentials;
- define/freeze the final v2 wire header or AAD;
- alter TLP v1 bytes;
- add secure RF;
- add ACK/COMMAND/MESSAGE runtime;
- add application GATT;
- change BLE admission policy;
- call `nRFCrypto.end()`;
- introduce a Crypto HAL/framework;
- claim thread-safety outside the exact tested path.

## 3. Candidate test vector

All values below are fixed public test material. No real device secret is used.

### 3.1 KDF

```text
K_root =
000102030405060708090a0b0c0d0e0f
101112131415161718191a1b1c1d1e1f

credential_id =
a0a1a2a3a4a5a6a7a8a9aaabacadaeaf

label = ASCII("ORUN-TLP-V2-AEAD")       // 16 bytes
key_epoch = 0x01020304

D2A info =
4f52554e2d544c502d56322d41454144
01
01020304

A2D info =
4f52554e2d544c502d56322d41454144
02
01020304

expected D2A AES-128 key =
b4db25a99bade834d006c0992d6dbe1a

expected A2D AES-128 key =
dea76f45a7abc04233848335b16a3ee1
```

### 3.2 Candidate nonce + probe-local AAD

```text
tx_counter = 0x1122334455667788

D2A nonce =
key_epoch_be32 || 0x01 || tx_counter_be64
=
01020304011122334455667788

probe AAD = ASCII("ORUN-M7P6E-KAT")
=
4f52554e2d4d375036452d4b4154
```

The AAD above is deliberately **probe-local**, not a proposed v2 header. It
exists only so the candidate key/nonce construction is exercised by AES-CCM
without prematurely allocating wire bytes.

### 3.3 CCM reference

AES-128-CCM, nonce 13 bytes, tag 8 bytes:

```text
plaintext =
000102030405060708090a0b0c0d0e0f10111213141516

ciphertext =
60ff0ec3e211ae143ca6c115c150bd6f05da47e416856b

tag =
bd9b12044081697b
```

The reference outputs were generated outside the target implementation with
Python `cryptography 46.0.4`. Final independent review must regenerate them
with an independent host implementation/tool before treating the candidate
contract as implementation-ready.

M7P6E does not allow a zero-length protected application payload, so no
zero-length CCM vector is required by this slice. A later wire specification may
make a different decision and must then add the corresponding vector.

## 4. Target KAT behavior

`firmware/tests/m7/m7p6e_crypto_ble_probe.cpp`:

1. derives the D2A key and checks the exact expected 16 bytes;
2. derives the A2D key and checks the exact expected 16 bytes;
3. requires the two keys to differ;
4. reconstructs the exact 13-byte D2A nonce;
5. encrypts the probe payload/AAD and checks exact ciphertext + 8-byte tag;
6. decrypts and checks exact plaintext;
7. flips one tag bit and requires the pinned expected auth-reject behavior;
8. clears the rejected output buffer;
9. performs an immediate valid decrypt recovery check.

The M7P6C pinned-binary compatibility rule remains narrow: only the known
wrong-tag Finish/decrypt path may accept either the dedicated MAC-invalid code
or the exact observed `CRYS_FATAL_ERROR`. No other operation reclassifies
arbitrary FATAL as authentication rejection.

All CC310 inputs passed to the target API are mutable RAM buffers.

## 5. Bluefruit lifecycle rule

The full probe image follows production startup order. Storage recovery still
occurs before `Bluefruit.begin()`.

After `Bluefruit.begin()` succeeds:

- M7P6E does **not** call `nRFCrypto.begin()`;
- M7P6E does **not** call `nRFCrypto.end()`;
- it runs the candidate KAT as the readiness canary;
- BLE advertising/admission then proceeds through the existing M7P7B path.

This specifically tests the lifecycle boundary required by M7P6D instead of
copying M7P6C's isolated begin/end pattern.

## 6. Coexistence stress

The probe observes three framework BLE security events in the existing direct
Bluefruit event callback:

- `BLE_GAP_EVT_LESC_DHKEY_REQUEST`;
- `BLE_GAP_EVT_AUTH_STATUS`;
- `BLE_GAP_EVT_CONN_SEC_UPDATE`.

The callback only increments counters under a critical section. It does not run
crypto, Serial, clocks, storage or policy.

With exactly one BLE client connected, serial command:

```text
CRYPTO STRESS
```

starts a bounded loop-task stress:

- maximum 3000 iterations;
- minimum 20 ms spacing between KAT starts;
- each iteration runs the complete candidate KAT;
- BLE must remain connected;
- any KAT failure fails immediately;
- a LESC DH-key event must be observed after stress starts;
- after the first overlapping LESC event, 100 further successful KAT iterations
  are required;
- final PASS reports LESC/auth/security-update deltas, disconnect delta and the
  maximum measured KAT duration.

This is intentionally stronger than merely running crypto while advertising: it
requires evidence that the Bluefruit security path reached its LESC DH-key work
during the active repeated ORUN crypto period.

It is **not** a proof of arbitrary future parallel CryptoCell use. It proves
only this pinned framework/library, this task structure and this bounded test
pattern.

## 7. Commands — test environment only

```text
CRYPTO?         -> probe state + BLE security-event counters
CRYPTO PROBE    -> one candidate KAT after Bluefruit is ready
CRYPTO STRESS   -> start bounded connected/pairing coexistence stress
```

These commands do not exist in the normal `rak4630` production image.

## 8. PlatformIO target

```text
env:rak4630_m7p6e_crypto_ble_probe
```

Unlike M7P6C's isolated target, this environment intentionally keeps the full
production source/dependency/patch graph and only adds:

- `-D ORUN_M7P6E_CRYPTO_BLE_PROBE=1`;
- `-I tests/m7`;
- the target-only probe translation unit.

The default environment remains `rak4630`.

## 9. Required validation sequence

Software:

1. full host regression suite;
2. normal production `pio run -e rak4630`;
3. probe `pio run -e rak4630_m7p6e_crypto_ble_probe`;
4. compare normal production RAM/flash with prior baseline; the probe image is
   test-only and its larger footprint is not production cost.

Hardware:

1. upload the M7P6E probe image;
2. boot monitor must show `M7P6E BOOT KAT PASS`;
3. connect exactly one phone;
4. start `CRYPTO STRESS`;
5. initiate a fresh BLE bond/pairing while the stress is active;
6. require `M7P6E LESC OVERLAP observed`;
7. require final `M7P6E COEX PASS ... disconnect_delta=0 ble_connected=1`.

If the peer silently reuses an existing bond and no LESC event occurs, the run
is **not** a coexistence PASS; establish a fresh pairing context and repeat the
stress. Do not weaken the gate to AUTH_STATUS alone.

After owner hardware evidence, run independent security/code review before
merge.

## 10. Software/build evidence

Owner-run validation on the M7P6E branch:

- full `firmware/tests/run_host_tests.sh`: **PASS**;
  the new M7P6E byte-contract check is silent on success but is included under
  `set -e`, so the complete suite reaching its final PASS lines means it
  completed successfully;
- normal production `pio run -e rak4630`: **PASS**;
- production resources remain exactly at the prior M6P2/M7P6C baseline:
  - RAM: 22,084 / 248,832 bytes = 8.9%;
  - Flash: 225,500 / 815,104 bytes = 27.7%;
- M7P6E full-graph probe build: **PASS**;
  - RAM: 22,132 / 248,832 bytes = 8.9%;
  - Flash: 238,292 / 815,104 bytes = 29.2%;
- probe-only delta versus production:
  - RAM: +48 bytes;
  - Flash: +12,792 bytes.

The visible SX126x `#warning USING RAK4630` and SimpleTimer signed/unsigned
warnings are existing third-party-library warnings, not new M7P6E warnings.

Independent local reference regeneration using Python `cryptography` reproduced
the exact candidate outputs recorded in §3:

- D2A key `b4db25a99bade834d006c0992d6dbe1a`;
- A2D key `dea76f45a7abc04233848335b16a3ee1`;
- nonce `01020304011122334455667788`;
- ciphertext `60ff0ec3e211ae143ca6c115c150bd6f05da47e416856b`;
- tag `bd9b12044081697b`.

This software/build evidence does **not** establish Bluefruit/CC310 hardware
coexistence. The physical LESC-overlap gate in §9 remains pending.

## 11. Compatibility / system impact

Normal production `rak4630` behavior is intentionally unchanged:

- TLP v1 TEST/POSITION/RELAY_FORWARD bytes unchanged;
- RF frequency/SF/BW/CR/TX power unchanged;
- relay forwarding/dedupe unchanged;
- tracker 10-second bounded RX unchanged;
- SecurityStore format and ownership unchanged;
- History/Config/BLE-bond partitions unchanged;
- BLE admission/runtime unchanged;
- GNSS/activity/geofence unchanged;
- no production secure packet path;
- no production CryptoCell call added.

No physical result is claimed until the owner runs the recorded hardware gate.
