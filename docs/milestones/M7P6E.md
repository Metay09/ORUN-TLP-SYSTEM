# M7P6E — CryptoCell + Bluefruit/SoftDevice coexistence proof

Status: **SOFTWARE/HOST/BUILD VALIDATION PASS; HARDENED FRESH-PAIRING LESC/CC310 COEXISTENCE HARDWARE PASS (SCOPED); PRODUCTION SECURE ENVELOPE STILL NOT ACTIVATED.**

Baseline: `main@00a96811c73cd5f9609f6869eb2c4a055013842b`
(PR #29 merged after M7P6D).

Branch: `feat/m7p6e-cryptocell-bluefruit-coexistence`.

## 1. Purpose

M7P6C proved the pinned CryptoCell primitives only in an isolated target image.
M7P6D then recorded the independently reviewed candidate security contract but
left one production blocker explicit: Bluefruit already initializes and uses the
same global `nRFCrypto` / CC310 facility, including LESC pairing work from a
different FreeRTOS task.

M7P6E is the smallest code-bearing slice designed to probe:

> Can the exact candidate ORUN HKDF/AES-CCM operation run in the real production
> source graph after SoftDevice/Bluefruit initialization, and can a bounded
> stress later exercise that path across a fresh LESC pairing interval?

The original M7P6E merge closed only the first question plus a bonded
BLE-connected coexistence check; fresh-pairing LESC stress was owner-waived at
that time. The later hardened probe on
`fix/m7p6e-pairing-evidence` was physically exercised with a fresh BlueZ
pairing while repeated candidate KATs were active and produced the complete
LESC/auth/bond/encrypted-link evidence plus the post-pairing KAT tail required
by §6. That later run closes the **specific pinned-path coexistence gate** that
M7P6D left open. It does not prove arbitrary CC310 thread-safety and does not,
by itself, make the production secure-envelope design ready for activation.

This is still a **test-only probe**. It does not put cryptography into the
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
- `BLE_GAP_EVT_AUTH_STATUS` must report `BLE_GAP_SEC_STATUS_SUCCESS`,
  report LESC for that completed authentication, and SoftDevice must report
  that the procedure resulted in a bond (this bit alone is not bond-persistence
  evidence);
- a post-start `BLE_GAP_EVT_CONN_SEC_UPDATE` must report an encrypted
  Security Mode 1 link (level >= 2);
- any post-start authentication failure or disconnect fails the run;
- only after all pairing-completion evidence above is present are 100 further
  successful KAT iterations required;
- final PASS reports LESC/auth/bond/encrypted-security-update deltas,
  disconnect delta and the maximum measured KAT duration.

This is intentionally stronger than merely running crypto while advertising: it
requires evidence that the Bluefruit security path reached its LESC DH-key work
during the bounded interval in which repeated ORUN KATs were being scheduled.

The counter-based observation does **not** prove instruction-level temporal
overlap between one specific Bluefruit CC310 call and one specific ORUN CC310
call. Even if this stress is later run successfully, it is evidence for the
pinned framework/task pattern, not a proof of arbitrary CryptoCell thread-safety.

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

## 9. Validation plan and owner disposition

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
7. require `M7P6E PAIRING COMPLETE ...` with non-zero successful,
   LESC+bonded authentication and encrypted-security-update deltas;
8. require final `M7P6E COEX PASS ... auth_failure_delta=0 disconnect_delta=0 ble_connected=1`.

If the peer silently reuses an existing bond and no LESC event occurs, the run
is **not** a fresh-pairing coexistence PASS. An AUTH_STATUS event alone is also
insufficient: pairing must complete successfully, produce the expected stock
bond, and reach an encrypted link before the post-pairing KAT window can close
PASS.

For the original M7P6E merge the owner explicitly waived steps 4-8 to avoid
disturbing the working bond state. That historical waiver is retained here as
part of the milestone record, but it is **superseded for this specific gate** by
the hardened fresh-pairing hardware run recorded in §10.2. The result is scoped
to the pinned RAK4630-class hardware/framework/probe path; a later production
secure-envelope milestone must still review scheduling, timeout and ownership
assumptions before enabling protected traffic.

No Astra/external independent review was available for the final M7P6E head.
A separate final static audit was performed after the owner waiver; its
documentation findings are recorded in §12.

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

At the time of the original software/build validation this evidence did **not**
establish fresh-pairing Bluefruit/CC310 coexistence, and that stronger gate was
owner-waived. The later hardened hardware run in §10.2 supersedes that historical
disposition for the specific pinned coexistence path.

### 10.1 Post-merge pairing-evidence hardening validation — code head `98c8486c9b1f089f706d7ed24566f5e3164e28c5`

Owner-run Debian validation of the hardened **test-only** pairing evidence gate:

- full `firmware/tests/run_host_tests.sh`: **PASS**;
  the new `test_m7p6e_pairing_evidence.cpp` check is intentionally silent on
  success and is included under the runner's `set -e`;
- normal production `pio run -d firmware -e rak4630`: **PASS**;
  - RAM: **22,116 / 248,832 bytes = 8.9%**;
  - Flash: **226,212 / 815,104 bytes = 27.8%**;
  - exactly unchanged from the merged M7P7D production image, so this
    test-only hardening adds **0 B production RAM / 0 B production Flash**;
- hardened full-graph probe
  `pio run -d firmware -e rak4630_m7p6e_crypto_ble_probe`: **PASS**;
  - RAM: **22,212 / 248,832 bytes = 8.9%**;
  - Flash: **240,476 / 815,104 bytes = 29.5%**;
- hardened probe-only delta versus the current production image:
  - RAM: **+96 bytes**;
  - Flash: **+14,264 bytes**;
- delta versus the earlier M7P6E probe image recorded above:
  - RAM: **+80 bytes**;
  - Flash: **+2,184 bytes**.

The visible SX126x `#warning USING RAK4630` and SimpleTimer
signed/unsigned warnings are from the existing third-party SX126x-Arduino
sources. The host suite still compiles ORUN-owned host-test targets with
warnings-as-errors plus ASan/UBSan.

Hardware upload and hardened boot/readiness were subsequently exercised on the
owner's RAK4630-class test unit. The device reported:

```text
M7P6E STATUS boot_kat=PASS stress=IDLE iterations=0 lesc_events=0 auth_events=0 auth_success=0 auth_failure=0 bonded_success=0 lesc_bonded_success=0 sec_update_events=0 encrypted_updates=0 ble_connected=0
```

This is scoped physical evidence that the hardened probe image booted and the
candidate KAT still passed after Bluefruit/SoftDevice initialization. It is **not**
fresh-pairing coexistence evidence by itself; no fresh pairing or stress run had
started at this point. The later §10.2 run supplies the missing fresh-pairing
evidence.

### 10.2 Hardened fresh-pairing hardware evidence — 2026-09-21

The hardened probe code exercised on hardware is the code-bearing head
`98c8486c9b1f089f706d7ed24566f5e3164e28c5`. The later branch commits through
`a1a19c36209162ff1af1b4f5d1591e7874ccaa0c` changed only this milestone
document, so they do not change the probe binary under test.

The RAK4630-class test unit booted the hardened probe with
`boot_kat=PASS`. A Debian BlueZ peer was observed as unpaired/unbonded before
the first pairing attempt. The first stress interval intentionally remains
recorded because it demonstrates the gate failing closed when the pairing was
triggered too late:

```text
M7P6E COEX FAIL reason=no-lesc-overlap iterations=3000 lesc_delta=0 auth_delta=0 auth_success_delta=0 bonded_success_delta=0 lesc_bonded_success_delta=0 auth_failure_delta=0 sec_update_delta=0 encrypted_update_delta=0 max_kat_us=1954
```

The pairing completed only after that interval and the status counters then
showed one LESC event, one successful authentication, one LESC+bonded success
and one encrypted security update. This late pairing was correctly **not**
promoted to PASS.

After removing the BlueZ peer locally, rediscovering it, reconnecting, and
starting a new stress interval, a fresh pairing was initiated while the stress
was active. BlueZ reported `Bonded: yes`, `Paired: yes` and
`Pairing successful`. The device-side evidence gate reported:

```text
M7P6E LESC OVERLAP observed iteration=1320
M7P6E PAIRING COMPLETE iteration=1350 auth_success_delta=1 bonded_success_delta=1 lesc_bonded_success_delta=1 encrypted_update_delta=1
M7P6E COEX PASS iterations=1450 lesc_delta=1 auth_delta=1 auth_success_delta=1 bonded_success_delta=1 lesc_bonded_success_delta=1 auth_failure_delta=0 sec_update_delta=1 encrypted_update_delta=1 disconnect_delta=0 max_kat_us=98632 ble_connected=1
```

A duplicate `CRYPTO STRESS` command during that same active run produced
`M7P6E COEX REJECT reason=already-active`; the probe returns immediately on
that path and does not reset or alter the already-running stress state, so this
does not invalidate the subsequent PASS.

This is **physical PASS for the scoped fresh-pairing coexistence gate**:
the pinned Bluefruit/SoftDevice security path reached LESC work, completed
successful LESC+bonded authentication, reached an encrypted Security Mode 1
link, remained connected with zero authentication failures/disconnects, and
then completed the required 100 additional candidate KAT iterations.

The observed `max_kat_us=98632` is also retained as a timing finding. In the
earlier no-pairing stress run the maximum was `1954 us`; during the successful
fresh-pairing run one KAT reached about **98.6 ms**. The run remained correct and
connected, so this is not a coexistence failure. It is **not** a characterized
worst-case bound and its cause is not proven to be CC310 contention. Future
production secure-envelope scheduling must therefore not assume the candidate
crypto operation always completes in ~2 ms; RF deadlines, timeout policy,
power transitions and ownership/serialization must tolerate or explicitly
bound such security-event latency.

This result does **not** prove arbitrary CryptoCell concurrency, all BLE peers,
durable bond persistence, ORUN application authorization, or production secure
packet readiness. BLE bond remains distinct from ORUN authorization.

## 11. Initial hardware evidence

Owner uploaded `rak4630_m7p6e_crypto_ble_probe` successfully to the RAK4630-class
test unit through `/dev/ttyACM0`.

After boot, the test-only `CRYPTO?` command reported:

```text
M7P6E STATUS boot_kat=PASS stress=IDLE iterations=0 lesc_events=0 auth_events=0 sec_update_events=0 ble_connected=0
```

This closes only the **hardware boot/readiness KAT** gate: the exact candidate
HKDF/AES-CCM KAT passed after Bluefruit/SoftDevice initialization on the real
device. It does not yet prove concurrent LESC/ORUN CryptoCell coexistence.

During the same probe-image session the unit also received a real direct TLP v1
packet:

```text
BASE RX NEW source=0E8ADE7E71531AA3 seq=11940 path=DIRECT rssi=-77 snr=9
```

That is scoped evidence that the full-graph probe image retained working direct
LoRa RX while the BLE runtime/SoftDevice was active. It is not a new range,
capacity or secure-RF claim.

At this initial-evidence stage, the stronger fresh-pairing LESC-overlap stress
had not been run and was owner-waived. That statement is historical: the later
hardened run in §10.2 physically exercised the fresh pairing and closed the
specific pinned-path coexistence gate.

The initial accepted physical evidence boundary was:
- boot/readiness KAT passed on real RAK hardware after Bluefruit/SoftDevice init;
- BLE connected successfully in the full probe runtime;
- a security update event was observed on the bonded connection;
- direct LoRa RX continued in the same full-graph probe image.

Section §10.2 later adds the missing fresh-pairing evidence. Even with that
addition, the milestone still does **not** prove arbitrary CC310 concurrency or
a universal timing bound; those broader scheduling/ownership risks remain
relevant to any future production secure-envelope activation.

A subsequent connection check on the same hardware reported:

```text
M7P6E STATUS boot_kat=PASS stress=IDLE iterations=0 lesc_events=0 auth_events=0 sec_update_events=1 ble_connected=1
```

This is valid physical evidence that BLE remained connected while the probe
runtime was active, but it is **not** the required fresh-pairing coexistence
evidence. The phone UI also showed the peer as BONDED, and the event pattern
(`sec_update_events=1`, `lesc_events=0`, `auth_events=0`) is consistent
with reuse of an existing bond/security context rather than a new LESC pairing.
Accordingly this bonded reconnect is not evidence for a fresh LESC pairing
path.

## 12. Final static audit

A final second-pass static audit of PR #30 found no production-runtime blocker.
The important findings were claim/evidence mismatches rather than runtime
defects:

1. the milestone still described fresh LESC overlap as a completed/required
   outcome after the owner had waived that physical test;
2. the stress wording used "overlap" too strongly even though the implementation
   observes a LESC event counter during a repeated-KAT interval, not
   instruction-level simultaneous CC310 execution;
3. several later sections still said the final fresh-pairing stress "must" run,
   contradicting the recorded owner waiver.

These documentation claims were corrected without changing runtime or test code.
The residual concurrency risk is intentionally **open** and blocks treating the
M7P6D candidate as production secure-envelope-ready.

No independent Astra/external reviewer result is claimed for M7P6E.

### 12.1 Post-merge pairing-evidence hardening — 2026-09-21

A later review found one false-positive path in the **test-only** stress logic:
the original implementation could print `M7P6E COEX PASS` after observing a
LESC DH-key event plus the post-LESC KAT window without proving that the pairing
procedure itself completed successfully.

The hardening on `fix/m7p6e-pairing-evidence` changes only the probe path:

- AUTH_STATUS success/failure is counted separately;
- successful AUTH_STATUS must also report LESC and SoftDevice's
  procedure-resulted-in-a-bond bit; this does not independently prove durable
  bond persistence;
- CONN_SEC_UPDATE must show encrypted Security Mode 1 (level >= 2);
- authentication failure and disconnect fail closed;
- the 100-iteration tail starts only after the complete pairing evidence is
  observed;
- BLE security events are re-sampled after each KAT so an event arriving during
  CC310 work is not missed at the terminal iteration;
- pure host coverage locks incomplete, rejected, disconnected, bonded-reconnect
  and uint32 counter-wrap cases.

The hardening alone did **not** change the owner waiver into PASS. The subsequent
hardware execution recorded in §10.2 did: the fresh-pairing LESC/CC310
coexistence gate is now **PASS for the scoped pinned path**, while the broader
production secure-envelope readiness questions remain separate.

### 12.2 Static review after hardened hardware run

A subsequent static review found one narrow false-positive/report-consistency
race in the **test-only** final PASS path. After the post-KAT snapshot had
already been checked for authentication failure/disconnect, the code read the
event counters a second time only for the PASS log. A failure/disconnect
arriving between those two snapshots could therefore be included in the printed
deltas while the code still emitted `M7P6E COEX PASS`.

Commit `84585ae843586cafbbb1ee7067845e378118122e` removes that second counter
read and reports the exact post-KAT snapshot that was already validated before
the PASS decision. This keeps the evidence line internally consistent with the
decision boundary and remains compiled only in the M7P6E probe image.

The physical PASS in §10.2 was obtained on code-bearing head
`98c8486c9b1f089f706d7ed24566f5e3164e28c5`, before this correction.
Therefore the branch is **not merge-ready** until the canonical host/build
validation and the scoped fresh-pairing hardware run are repeated on the new
code head. No production runtime behavior changed.

### 12.3 Post-fix software/build revalidation — head `e408b0b0135e62a5dc2dd5b70f2d825dfc369863`

After the §12.2 PASS-snapshot correction, the owner fast-forwarded the branch to
`e408b0b0135e62a5dc2dd5b70f2d825dfc369863` and repeated the canonical
software/build validation:

- full `bash firmware/tests/run_host_tests.sh`: **PASS** through the final R4
  watchdog checks; this includes the M7P6E crypto-contract and pairing-evidence
  host tests under the canonical sanitizer/warnings-as-errors runner;
- normal production `pio run -d firmware -e rak4630`: **PASS**;
  - RAM: **22,116 / 248,832 bytes = 8.9%**;
  - Flash: **226,212 / 815,104 bytes = 27.8%**;
  - exactly unchanged from the merged M7P7D production baseline;
- hardened full-graph probe
  `pio run -d firmware -e rak4630_m7p6e_crypto_ble_probe`: **PASS**;
  - RAM: **22,212 / 248,832 bytes = 8.9%**;
  - Flash: **240,364 / 815,104 bytes = 29.5%**;
  - versus the pre-§12.2 hardened probe build, RAM is unchanged and Flash is
    **112 bytes smaller**;
- both builds completed the application-ceiling and exclusive-owner link guards.

This revalidation proves the §12.2 change compiles and preserves the production
resource baseline. The branch still requires one scoped fresh-pairing hardware
rerun on this corrected code before the earlier §10.2 physical PASS can be
treated as revalidated for the current head.

### 12.4 Serial DFU upload diagnostic on corrected probe

The first post-fix `nrfutil` upload attempts failed after the 1200-bps touch
with a timeout / closed-port error even though PlatformIO printed a final
`[SUCCESS]`. During diagnosis, a separate `pio device monitor` process was
observed repeatedly reconnecting to `/dev/ttyACM0`. After that monitor was
stopped and `fuser -v /dev/ttyACM0` showed no owner, the same normal
PlatformIO upload path was retried without any manual reset:

```text
Forcing reset using 1200bps open/close on port /dev/ttyACM0
Waiting for the new upload port...
Uploading .../firmware.zip
Upgrading target on /dev/ttyACM0 ...
Activating new firmware
Device programmed.
```

The corrected probe therefore **did upload successfully through the normal
serial DFU path**. This is strong evidence that the earlier failures were a
host-side serial-port ownership/reconnect race rather than a firmware,
bootloader-layout or M7P6E crypto-probe regression. One successful retry does
not characterize all Linux/udev timing behavior, so future upload tooling should
still treat `Device programmed.` (or equivalent tool success) as the positive
completion signal rather than PlatformIO's outer `[SUCCESS]` line alone.

## 13. Compatibility / system impact

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

The recorded hardware results now include the hardened fresh-pairing
LESC/auth/bond/encrypted-link coexistence PASS in §10.2 in addition to the
earlier boot/readiness, bonded-connection and full-graph LoRa RX evidence.
This remains a scoped pinned-path result, not a claim of arbitrary CryptoCell
thread-safety or a production secure-envelope readiness verdict.
