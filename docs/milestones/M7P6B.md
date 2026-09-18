# M7P6B — SecurityStore + TX Nonce Persistence

Status: **IMPLEMENTED — software/host/build validated; limited real-hardware persistence/reboot validation PASS; PR #18 open, not merged.**

Baseline: `main@003a891a2b6e66c267e68cee2e704860f97bed69`

Final implementation SHA before docs-only closeout:
`6d3009d42d9fb36026be5379171d8994a71dbaf6`

This slice implements only durable security credential and TX nonce/counter
state. It does **not** implement a secure RF envelope, cryptography,
provisioning transport, authenticated ACK/contact, commands, BLE runtime,
backend/mobile integration or user authorization.

## 1. Product invariant

The primary invariant is:

> A TX security counter/nonce must never be reissued under the same security
> credential/key lifetime.

Unused reserved counters may be skipped after reset. Counter reuse is forbidden.

The security counter is independent of:

- TLP v1 sequence numbers;
- HistoryStore sequence/ticket reservation;
- ConfigStore state;
- BLE bond storage.

TLP v1 bytes remain unchanged and unauthenticated.

## 2. Partition and ownership

SecurityStore exclusively owns:

```text
0x0E7000..0x0E8000  page A
0x0E8000..0x0E9000  page B
```

Canonical map remains:

```text
0x000000..0x026000  MBR + SoftDevice
0x026000..0x0E7000  application policy region
0x0E7000..0x0E9000  SecurityStore
0x0E9000..0x0EB000  ConfigStore
0x0EB000..0x0ED000  bonds/InternalFS
0x0ED000..0x0F4000  HistoryStore
0x0F4000..0x100000  bootloader/settings
```

No partition overlaps were introduced.

## 3. SecurityStore v1 format

All fields use explicit byte serialization; native C++ structs are not persisted.

### Page header — 32 bytes

```text
offset  size  field
0       4     magic = 0x4F525331 ("ORS1")
4       1     format version = 1
5       3     reserved = 0
8       8     page generation
16      8     DeviceIdentity legacy uint64 binding
24      4     CRC32 over bytes [0..24)
28      4     page activation commit word = 0
```

The page-header commit word is the **A/B activation marker**. While a new page
is being assembled it remains erased. SecurityStore writes and verifies the
header body, the credential, and the carried-forward TX reservation snapshot
when required; only then is the header activation word programmed. Therefore
a power cut before activation leaves the older committed page authoritative.

### CREDENTIAL — 68 bytes

```text
offset  size  field
0       16    credential_id
16      4     key_epoch
20      8     DeviceIdentity legacy uint64 binding
28      32    K_root
60      4     CRC32
64      4     record commit word = 0
```

`credential_id` is 128 random bits. This is a conventional large identifier
for one security/provisioning lifetime; accidental reuse is negligible for
the expected product lifetime without wasting additional flash.

`K_root` is exactly 256 random bits stored raw. M7P6B does not invent at-rest
obfuscation/encryption. Physical secret extraction remains an unresolved
product-security boundary.

Re-provisioning is a new security lifetime. The store refuses an immediate
re-provision that reuses the current `credential_id` or the currently active
`K_root`, because resetting the TX counter to zero under the same lifetime/key
could permit nonce reuse. Preventing reuse of an older historical root is a
future provisioning-layer responsibility; SecurityStore intentionally retains
only the current credential.

### TX_RESERVE — 36 bytes

```text
offset  size  field
0       16    credential_id
16      4     key_epoch
20      8     absolute exclusive TX reserved bound
28      4     CRC32
32      4     record commit word = 0
```

A TX_RESERVE record is valid only when its bound is non-zero and an exact
multiple of 256.

Each page contains:

- 32-byte header;
- one 68-byte CREDENTIAL slot;
- 111 TX_RESERVE slots × 36 bytes.

Total: 4096 bytes exactly.

## 4. TX reservation semantics

Reservation block size is **256 counters**.

The durable value is an **exclusive upper bound**:

```text
durable bound = 256  => counters 0..255 may be used
durable bound = 512  => counters 256..511 may be used next
```

Before any counter from a new block is exposed:

1. the new absolute bound is encoded;
2. record body/CRC is written;
3. its commit word is written last;
4. physical completion is observed;
5. committed bytes are read back and verified;
6. only then is `tx_reserved_bound_` advanced in RAM.

At reboot, SecurityStore does not attempt to reconstruct the exact last-used
counter from RAM. It sets `tx_next` to the last durable bound, discarding
any unused counters below that bound, and durably reserves a fresh block before
returning another counter.

Counter overflow/wrap fails closed. No rollover or key-rotation protocol is
invented here.

## 5. Recovery states

The bounded state model is:

- `UNPROVISIONED`: blank/incomplete unactivated store; no credential;
- `PROVISIONED`: valid current credential and unambiguous nonce state;
- `FOREIGN`: structurally valid page bound to another DeviceIdentity;
- `UNSUPPORTED`: recognized future/newer security format;
- `FAULT`: backend unusable or committed security state is ambiguous/corrupt
  such that protected TX cannot safely continue.

A blank store does not disable existing TLP v1 tracking and does not create
credentials automatically.

### Fail-closed rules added by independent audit

Independent review after the original implementation found rollback hazards
that were fixed before PR closeout:

1. **A/B compaction activation ordering**
   - Original implementation committed a higher-generation page header before
     the carried-forward TX high-water mark was guaranteed durable.
   - A reset at that point could select the newer page with a zero/lower bound.
   - Fix: the page-header commit word is now the final activation write after
     the entire new-page snapshot is durable.

2. **Future-format downgrade**
   - If any page has recognized security magic with an unsupported version,
     the store returns `UNSUPPORTED` globally instead of falling back to an
     older v1 page whose counter/key state might be stale.

3. **Corrupt committed reservation state**
   - A non-erased invalid TX_RESERVE record on the authoritative page is not
     skipped in favor of an older/lower bound. The store returns `FAULT`.

4. **Append-log holes**
   - An erased TX_RESERVE slot followed by later non-erased records is
     impossible in legitimate append order and returns `FAULT`.

5. **Committed header corruption**
   - A non-erased page with a non-erased activation word but corrupted
     current-format header/magic is treated as ambiguous committed state and
     returns `FAULT`, rather than silently appearing blank.

These changes preserve legacy TLP v1 operation at the product composition
boundary but refuse protected TX counters from ambiguous security state.

## 6. Compaction

SecurityStore uses two-page A/B ping-pong.

Normal compaction:

1. erase inactive destination page at `SEC_MAINT` priority;
2. write/verify new header body while activation word remains erased;
3. write/commit carried-forward current TX bound in slot 0;
4. write/commit credential;
5. program/read-verify the page-header activation word **last**;
6. only then switch RAM authority to the new page;
7. erase superseded old page at `SEC_MAINT`;
8. append the next critical TX reservation.

A reset before step 5 leaves the old page authoritative. A reset after step 5
can select the new page because the complete snapshot was already durable.

The old and new pages are never erased together.

## 7. FlashMutationGate integration

M7P6B adds one Security owner with two priority-tagged views over the same
physical slot/staging buffer.

Admission order is:

```text
SEC_CRITICAL > History > Config > SEC_MAINT
```

Examples:

- `SEC_CRITICAL`: TX_RESERVE append and new credential snapshot writes that
  block protected security progress;
- `SEC_MAINT`: destination/old-page erase and ordinary compaction work.

A new-page erase remains `SEC_MAINT` even when the following credential
snapshot is critical; the port switches to `SEC_CRITICAL` only after the
fresh page erase completes.

Timeout begins only when the request receives the physical mutation slot, not
when queued.

The gate keeps one outstanding request per owner, one physical Nordic flash
mutation in flight, fixed staging buffers, no heap, and one SoftDevice event
drain.

To prevent indefinite starvation, a staged but unadmitted request older than
the existing 4-second operation budget receives temporary top admission
priority. This aging changes admission order only; it cannot preempt a physical
operation already accepted by Nordic.

Bond/InternalFS flash is still not routed through this gate. That remains a
later BLE/SoftDevice integration prerequisite.

## 8. Wear/capacity arithmetic

Each page physically contains 111 TX_RESERVE slots.

SecurityStore deliberately compacts before all 111 are consumed, leaving one
slot of deterministic headroom:

- initial page: up to 110 advancing reservation records;
- steady-state page after compaction: slot 0 carries the previous durable
  bound and up to 109 new advancing reservation records before next compaction.

At 256 counters/block:

- initial page represents up to 28,160 secure transmissions before compaction;
- steady-state between compactions represents about 27,904 secure transmissions.

Illustrative time between steady-state compactions if there is exactly one
secure TX per reporting interval:

| Reporting interval | Approx. time / compaction |
| --- | ---: |
| 3 min | 58.1 days |
| 15 min | 290.7 days |
| 30 min | 581.3 days |

These are arithmetic estimates, **not physical wear measurements**. Extra
events/commands increase reservation consumption. Reboots also skip the
remainder of one reservation block and require another durable reservation,
so pathological reset frequency can increase wear.

A successful compaction erases the destination page and later the superseded
old page; over alternating A/B operation this is roughly one erase per physical
security page per compaction. Even at a 3-minute illustrative secure-TX cadence,
that is roughly 63 compactions/page/year, or about 630 erase cycles/page over
10 years, before accounting for additional traffic/resets. Actual flash-endurance
margin must use the silicon/vendor endurance specification and field workload;
this milestone does not convert this arithmetic into a hardware-life guarantee.

## 9. Compatibility

Unchanged:

- TLP v1 packet bytes and golden fixtures;
- POSITION bytes;
- RELAY_FORWARD bytes;
- one-hop relay behavior;
- legacy DeviceIdentity values;
- HistoryStore journal format/semantics;
- ConfigStore format/schema/semantics;
- tracking interval behavior;
- role behavior;
- GNSS scheduling;
- RF listen/TX behavior;
- BLE remains OFF.

No existing compatibility fixture was weakened.

## 10. Validation evidence

Final source implementation validated at:

`6d3009d42d9fb36026be5379171d8994a71dbaf6`

### Host

Command:

```bash
./firmware/tests/run_host_tests.sh
```

Result: **PASS**.

The full suite passed under `-Wall -Wextra -Werror` and ASan/UBSan, including:

- legacy packet golden/malformed fixtures;
- M7P3 async history gate/integration;
- M7P5 ConfigStore/gate/runtime interval tests;
- M7P6B format tests;
- M7P6B SecurityStore fault-injection/property tests;
- M7P6B three-client priority/aging tests;
- production startup scenarios;
- R2/R4 radio/I2C/watchdog regressions.

M7P6B deterministic tests cover blank/recovery, successful credential commits,
torn writes, reboot skip-ahead, reservation boundaries, DeviceIdentity mismatch,
unsupported versions, counter overflow, compaction, interrupted compaction,
committed corruption, reserve gaps, same-lifetime/root re-provision refusal and
repeated-reset no-duplicate-counter properties.

### RAK4630 build

Command:

```bash
pio run -d firmware -e rak4630
```

Result: **SUCCESS**.

```text
RAM   15,420 / 248,832 B  (6.2%)
Flash 158,128 / 815,104 B (19.4%)
```

Baseline M7P5 reference:

```text
RAM   14,836 B
Flash 151,048 B
```

Delta:

```text
RAM   +584 B
Flash +7,080 B
```

The build also passed the existing exclusive-owner/application-ceiling/layout
guards.

## 11. Physical-validation boundary

### Real RAK4631 persistence/reboot evidence — PASS

A dedicated temporary physical-test image, isolated from the production PR
(`test/m7p6b-physical-sentinel@3078bb88befa0e6d6fc79f4f9ec39da0f94dcb5f`),
was uploaded over USB/serial DFU to the connected RAK4631.

The harness used a fixed **synthetic test-only credential**, never a production
credential, and exercised the real `NrfSecurityFlash` backend with SoftDevice
disabled:

1. boot recovered `UNPROVISIONED`;
2. synthetic credential was committed to real nRF52840 internal flash;
3. first 256-counter reservation completed with readback;
4. the harness internally required first counter == 0 and epoch == 1 before
   emitting `STAGE1 PASS`;
5. `NVIC_SystemReset()` performed a real MCU software reset;
6. second boot recovered `PROVISIONED` and matched the synthetic credential;
7. reboot recovery auto-reserved the next block; the harness required the
   returned counter to be >= 256, exactly divisible by 256, and epoch == 1
   before emitting `STAGE2 PASS`;
8. both SecurityStore pages were erased;
9. full security-region readback confirmed every byte erased before
   `CLEANUP PASS security_region=ERASED`.

Observed serial evidence:

```text
M7P6B PHYS recovered_state=UNPROVISIONED
M7P6B PHYS STAGE1 PASS
M7P6B PHYS software_reset=NOW
...
M7P6B PHYS recovered_state=PROVISIONED
M7P6B PHYS stage2=RECOVERY_TEST_CREDENTIAL_MATCH
M7P6B PHYS STAGE2 PASS
M7P6B PHYS CLEANUP PASS security_region=ERASED
M7P6B PHYS COMPLETE
```

The Adafruit/TinyUSB `Serial.printf` implementation used by this temporary
harness did not render the C `ll` length modifier correctly, so the printed
64-bit DeviceIdentity/counter text appeared as literal `lX`/`lu`. This is
a **diagnostic formatting defect in the temporary harness only**. The PASS
conditions above compare the actual integer values in firmware before printing,
so the malformed text does not weaken the persistence/reboot result.

Therefore the following are now physically validated on one real RAK4631:

- SecurityStore synchronous internal-flash write/readback;
- credential persistence across MCU software reset;
- reboot skip-ahead / no reuse of the partially used reservation block;
- cleanup erase + erased-region readback.

Still **not physically validated**:

- electrical power-cut/brownout during credential/reservation/compaction stages;
- SoftDevice-enabled asynchronous security flash completion;
- preservation of Security/Config/Bond regions across every actual update/DFU
  path (this test used serial DFU to install the harness but did not place
  sentinels in all lower persistence partitions before an update);
- real bootloader signature/rollback/bank behavior;
- physical secret extraction resistance;
- production credential provisioning.

No production credential was provisioned.

Host fault injection/build evidence and the limited hardware evidence above must
remain reported separately; this does not constitute electrical power-cut or
SoftDevice/BLE validation.

## 12. Explicitly deferred

Not implemented here:

- AES/CCM/HKDF;
- secure RF envelope/version;
- RX replay HWM;
- authenticated ACK/contact;
- command framework or command IDs;
- permissions/delegation;
- backend/app security integration;
- BLE runtime/commissioning;
- remote credential reset;
- firmware signing;
- LoRa DFU;
- low-VBAT security heuristics.

The next security protocol slice must consume this store without changing TLP v1
bytes and must separately freeze cryptographic libraries, vectors, on-air nonce/
AAD/header representation and mixed-fleet behavior.
