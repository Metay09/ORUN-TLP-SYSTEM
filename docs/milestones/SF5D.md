# SF5D — nRF52840 Flash Ownership + DFU Preflight

Status: **SF5D3 SAMPLED SERIAL DFU RETENTION: PHYSICAL PASS (3 pages, one update).**
**Gates D1/D2 remain OPEN** for the proposed full 128 KiB production store and
combined Tracker/Gateway ownership. No production 128 KiB reservation or
`FlashBackend`, no linker/build-ceiling change, and no ObservationStore/CustodyStore
runtime attachment. The test-only 16-byte marker did program one approved
unallocated development flash page; this is NOT full SF5D/SF5E clearance.

Baseline: `main@b9a97630bfd0c842dcca8ec332f80f25756b81be`
(SF5C portable-only squash merge of PR #83).

Canonical inputs:
- `AGENTS.md`, `docs/milestones/SF5.md`;
- `docs/architecture/ORUN_TLP_V2_PRODUCT_OBSERVATION_STORAGE_CUTOVER.md`;
- `docs/architecture/ORUN_GATEWAY_DURABLE_CUSTODY.md`;
- `firmware/include/storage_config.h`;
- `firmware/scripts/check_storage_layout.py` and
  `firmware/scripts/storage_layout_policy.py`;
- `firmware/include/flash_backend.h`,
  `firmware/include/flash_mutation_gate.h`, and their nRF implementations.

## 1. Verified repository facts (not device verification)

The RAK4630 target selects `nrf52840_s140_v6.ld`, S140 v6.1.1,
and settings address `0xFF000`. `check_storage_layout.py` verifies that
the installed framework's linker declares application FLASH
`[0x026000, 0x0ED000)` and bootloader address `0x0F4000`.
Its *separate post-link policy* currently requires image LOAD sections to end
at/below `0x0E5000`, from `storage_config.h`. The linker maximum is **not**
proof that everything up to that limit is empty, update-staging space, or
safe to erase. No firmware source modification may silently lower either bound.

The owner-approved **candidate** (NOT yet reserved) is
`[0x0C5000, 0x0E5000)`, 128 KiB = 32 * 4096-byte pages. This sits
immediately before the existing regions:

| Address range (end exclusive) | Current/contracted responsibility |
| --- | --- |
| `0x0C5000..0x0E5000` | Candidate 128 KiB product durable store; **unassigned** |
| `0x0E5000..0x0E7000` | Geofence |
| `0x0E7000..0x0E9000` | Security |
| `0x0E9000..0x0EB000` | Config |
| `0x0EB000..0x0ED000` | BLE/InternalFS |
| `0x0ED000..0x0F4000` | Legacy History |
| `0x0F4000..0x100000` | Bootloader/settings classes |

At the SF5C validated production-image measurement: RAM 29,536 B;
flash 285,924 B. A *hypothetical* new application/update span
`[0x026000, 0x0C5000)` has 651,264 bytes. **If** a specific installed
bootloader actually uses two equal image banks in that span, the arithmetic
is 325,632 bytes/bank; current measurement would leave only 39,708 B
per bank. This is a **budgeting scenario, not a DFU compatibility proof**.

## 2. Gate D1 — actual bootloader/DFU model, NOT YET CLOSED

The Adafruit nRF52 Bootloader upstream supports dual-bank firmware updates
but documents the feature as **disabled by default**. RAK4631 board/BSP
metadata, the linker script and a successful `pio run` do not establish
which bootloader revision, DFU command/options, scratch-page policy or bank
layout is **actually installed on the user's development device**.
A different RAK/RUI3 bootloader variant is not interchangeable evidence.

Source: https://github.com/adafruit/Adafruit_nRF52_Bootloader#features
and its `Dual bank firmware support` section.

Before changing `kApplicationPolicyEndAddress` or admitting **any** writes:
1. Identify exact bootloader family/build/version of the *actual board* by
   **read-only** means and match it to reproducible upstream source or binary.
2. Establish real serial/BLE DFU update algorithm: single vs dual bank,
   temp-storage location, page erasures, preservation of application-reserved
   pages, interrupted update and recovery behavior. A bootloader merely
   *supporting* two banks does not establish they are enabled.
3. Check headroom using **actual image LOAD addresses**, not only PlatformIO's
   headline flash-percentage metric. Preserve space for reviewed future growth;
   do not assume LoRa FOTA exists.
4. Demonstrate no overlap with Geofence/Security/Config/BLE/History,
   bootloader/settings and any DFU staging/rollback pages under the
   actual update model.
5. Record the device-specific evidence. If access to bootloader internals is
   unavailable, mark the gate OPEN; no guessed workaround, linker rewrite,
   erase or firmware upload.

### D1 source evidence — RAK4631 bootloader (2026-10-09)

**Confirmed from RAKwireless official documentation, not from the user's
installed board:**

- RAK's current bootloader update manual documents **v0.4.4**,
  S140 **6.1.1**, dated **2026-06-17**. It documents USB/serial/BLE
  methods; the Linux serial example invokes `adafruit-nrfutil` with
  `--singlebank --touch 1200`. A command-line option in an example
  is evidence of the documented update path, **not a readout of the
  installed bootloader's configuration**.
- The same manual explicitly cautions that **updating the bootloader
  overwrites the current application**. No bootloader replacement,
  firmware erase, flash relocation, or experimental DFU is authorized
  for this evidence-gathering step.
- The RAK source directory currently published under `Latest/`
  contains an older `Makefile` identifying its own `GIT_VERSION` as
  **0.4.3**. For its `nrf52840` build case, this source configures
  `DFU_APP_DATA_RESERVED=10*4096` (40 KiB), and its
  `src/usb/uf2/uf2cfg.h` defines
  `USER_FLASH_END = BOOTLOADER_REGION_START - DFU_APP_DATA_RESERVED`.
  Given its `0xF4000` bootloader start, that source's nominal
  reserved tail is **[0x0EA000, 0x0F4000)**. Its
  `dfu_single_bank.c` erases/writes app image pages in bank 0.
- **Do not equate** the published older source or upstream Adafruit
  defaults with the **0.4.4 binary**, and do not equate any of them
  with the **unknown revision physically installed** on the device.
  Version-specific build macros, DFU staging locations and erase
  boundaries must be established from the matching source/binary or
  targeted nondestructive tests before authorizing a new partition.

This exposes a **real compatibility gate**, not proof of data loss:
if an installed bootloader had the older 40 KiB app-data reservation
and performed an update that wrote into the rest of application flash,
the proposed [0x0C5000,0x0E5000) ObservationStore would **not** lie
inside that tail reservation. Nor would all of the current Geofence,
Security and Config pages. Current normal firmware images are much
smaller than that address, and no destructive/OTA update test was
performed here; do not claim actual erasure of existing data.
The previously computed hypothetical 325,632-byte dual-bank budget
**is not a proven operating mode**; retaining 128 KiB without update
corruption may require an update-compatible memory contract not
provided by the installed bootloader.

Primary vendor evidence (source and documentation inspected directly):
- RAK bootloader update manual:
  https://github.com/RAKWireless/WisBlock/blob/master/bootloader/RAK4630/README.md
- RAK-published source Makefile (older `0.4.3` tree):
  https://github.com/RAKWireless/WisBlock/blob/master/bootloader/RAK4630/Latest/WisCore_RAK4631_Bootloader/Makefile
- Its UF2 application limit:
  https://github.com/RAKWireless/WisBlock/blob/master/bootloader/RAK4630/Latest/WisCore_RAK4631_Bootloader/src/usb/uf2/uf2cfg.h
- Its Nordic DFU code:
  https://github.com/RAKWireless/WisBlock/blob/master/bootloader/RAK4630/Latest/WisCore_RAK4631_Bootloader/lib/sdk11/components/libraries/bootloader_dfu/dfu_single_bank.c
- Upstream Adafruit bootloader documentation:
  https://github.com/adafruit/Adafruit_nRF52_Bootloader

**Next safe evidence action when needed:** identify the installed bootloader
version/board ID through `INFO_UF2.TXT` in the device's read-only UF2
drive, as explicitly documented by RAK. Entering UF2 mode is not
permission to copy/update any firmware. This version alone **does not
prove data-page preservation**; the next investigation must correlate
the matching binary/implementation and actual update policy. If the
required evidence cannot be obtained, Gate D1 remains OPEN and
production storage writes are forbidden.

### D1 read-only source-geometry preflight (SF5D1; not D1 clearance)

`firmware/scripts/sf5d_layout_preflight.py` verifies that the **unassigned**
candidate interval `[0x0C5000,0x0E5000)` abuts the existing geofence region
and that all current Geofence/Security/Config/BLE bond/legacy History intervals
remain page-aligned, contiguous, and correctly bounded below the bootloader.
It reuses the existing fail-closed application-ceiling parser and checks the
corresponding active C++ declarations/static assertions. Changing the physical
owner geometry without coordinated review must reject.

`firmware/tests/sf5/test_sf5d_flash_layout_preflight.py` exercises the
current source and deliberately altered source fixtures (security/config
growth, relocated geofence, altered ceiling/alias, broken history boundary).
The check runs as part of `firmware/tests/run_host_tests.sh` and is independently
runnable from the project root:

```sh
PYTHONDONTWRITEBYTECODE=1 python3 firmware/scripts/sf5d_layout_preflight.py
PYTHONDONTWRITEBYTECODE=1 python3 firmware/tests/sf5/test_sf5d_flash_layout_preflight.py
```

**Non-claims:** This parses only repository source; it does not interrogate
physical flash, installed bootloader, actual DFU erasure policy, linker LOAD
sections, or capacity for two separate durable stores. The existing production
application ceiling remains `0x0E5000`; the proposed candidate remains
**unallocated**. D1/D2 remain OPEN. Firmware/bootloader upload, reset,
erase, reformat, physical qualification and runtime cutover are excluded.

### SF5D2 — installed bootloader version handoff (TEST-ONLY, D1 still OPEN)

The temporary PlatformIO environment `rak4630_sf5d_bl_version_probe` keeps
the production sources and normal storage/LoRa behavior unchanged. It adds
one read-only diagnostic to `main.cpp` behind a build macro. The pinned
Adafruit nRF52 Arduino startup reads a version value handed off through the
bootloader's TIMER2 CC[0] register into `bootloaderVersion` before `setup()`.
RAK's older published 0.4.3 bootloader source encodes the number as
`(major << 16) | (minor << 8) | patch`. **A result is an untrusted version
hint, not proof of the exact bootloader binary or DFU page-erasure policy.**

On an **approved development device only** (from `firmware/`):

```sh
pio run -e rak4630_sf5d_bl_version_probe
pio run -e rak4630_sf5d_bl_version_probe -t upload --upload-port /dev/ttyACM0
pio device monitor -b 115200 -p /dev/ttyACM0
```

`ttyACM0` is illustrative; re-resolve the selected USB-by-id symlink before
every flash and select the correct **test device**, never rely on port order.
In the test image serial monitor, send `SF5D BL?` followed by Enter (or
observe the startup `SF5D BL VERSION` line). The query does not change device
flash, reset the MCU or enter DFU. The `pio ... upload` step **does** program
the test-device firmware using serial DFU and can alter application flash;
do not use it on deployed devices. Record the reported handoff value together
with which bootloader family/source can actually be substantiated.

**RAK-1 physical observation (2026-10-09, operator report):**
- Target USB unique serial: `0E8ADE7E71531AA3` (TRACKER).
- `pio run -e rak4630_sf5d_bl_version_probe`: **PASS**, RAM 29,536 B,
  Flash 286,308 B; 94.02 s.
- `pio run -e rak4630_sf5d_bl_version_probe -t upload`:
  **PASS**, serial nrfutil single-bank, Device programmed; 34.25 s.
- Live read-only `SF5D BL?` returned:
  `SF5D BL VERSION handoff=0x00000402 candidate=0.4.2 evidence=core-handoff-only`.
- After upload, operator saw GNSS FIX, TX POSITION and STORAGE appended
  diagnostics; RF receiver confirmation in this test was not established.
- **Version-handoff 0.4.2 is not attestation of the installed binary, nor
  proof of what pages a later DFU actually erases or preserves. Gate D1 OPEN.**
  The candidate `[0x0C5000,0x0E5000)` was not read, written or erased.

The two current RAK4631 units are development devices; the owner accepts
loss of **existing test records** for deliberate qualification. This does
**not** relax the future product requirement that updates preserve durable
user observations. It is also not permission to erase bootloader/SoftDevice,
change production application boundaries or activate the unallocated
128 KiB region. Gate D1 must be closed with observed update/erase behavior
before any production storage writes.

### SF5D3 — mixed-content DFU sentinel-retention physical experiment (TEST ONLY)

**Owner-approved DEVELOPMENT device RAK-1, bootloader handoff 0.4.2.**
This experiment does NOT assign production flash ownership. It does NOT
permit changes to the application linker boundary, bootloader/SoftDevice,
existing protected stores or deployed devices.

**Operator's first physical observations (2026-10-09):**
- Host source-contract checks: **PASS**; both isolated target builds:
  `rak4630_sf5d_dfu_seed` **SUCCESS** (Flash 56,164 B / RAM 8,776 B);
  `rak4630_sf5d_dfu_verify` **SUCCESS** (Flash 55,372 B / RAM 8,776 B).
  The first SEED build previously failed an original M4 primitive link guard;
  the corrected exact-target-only guard preserved production requirements.
- First SEED firmware upload to RAK-1 unique serial `0E8ADE7E71531AA3`:
  **SUCCESS** 8.913 s, serial nrfutil single-bank.
- Without any explicit marker-write command, boot/read-only `SF5D STATUS`:
  - page `0x0C5000`: **OTHER**, first word `0x2578303D`;
  - page `0x0D5000`: **OTHER**, first word `0x09091701`;
  - page `0x0E4000`: **ERASED**, first word `0xFFFFFFFF`;
  - `matched=0/3 erased=1/3 result=MISMATCH` under the **original
    all-three-erased precondition**, not data corruption evidence.
- The original all-three marker seed was not run; its code would reject
  because the first two pages were not erased. No test marker was reported
  written. We MUST NOT automatically erase/reinitialize these pages.

**Revised SEED physical pre-write baseline (operator report, 2026-10-09):**
- Fresh revised SEED firmware upload to RAK-1: **SUCCESS**, 8.657 s,
  serial nrfutil reporting `Single bank`.
- Repeated independent `SF5D STATUS` reads were identical:
  - `0x0C5000` OTHER first=`0x2578303D`,
    whole-page CRC32=`0x8FCBBCDC` (read-only control).
  - `0x0D5000` OTHER first=`0x09091701`,
    whole-page CRC32=`0x1CAE7BAD` (read-only control).
  - `0x0E4000` ERASED first=`0xFFFFFFFF`,
    whole-page CRC32=`0xF154670A` (sole permitted seed page).
  - `matched=0/3 erased=1/3 seed_address=0x0E4000 seed_state=ERASED`.
- All physical reads are **before** `SF5D SEED CONFIRM`.
  No successful marker-write or post-DFU comparison reported yet.

**RAK-1 physical SEED write result (operator report, 2026-10-09):**
- Explicit `SF5D SEED CONFIRM` yielded
  `SF5D DFU SEED PASS one-page-readback`.
- After the single 16-byte marker write:
  - `0x0C5000` OTHER first=`0x2578303D`
    CRC32=`0x8FCBBCDC` (**unchanged**).
  - `0x0D5000` OTHER first=`0x09091701`
    CRC32=`0x1CAE7BAD` (**unchanged**).
  - `0x0E4000` MATCH first=`0x53463544`
    CRC32=`0xAED32820` (**new expected marker**).
  - `matched=1/3 erased=0/3 seed_address=0x0E4000 seed_state=MATCH`.
- This is a physical SEED PASS and the **pre-DFU CRC baseline**.
  At this stage VERIFY was still pending; the subsequent matched post-DFU
  readout is documented below.

**RAK-1 physical VERIFY / actual serial DFU retention result (operator report, 2026-10-09):**
- `pio run -e rak4630_sf5d_dfu_verify -t upload` on the same RAK-1
  USB identity: **SUCCESS**, 8.636 s; nrfutil reported `Single bank`,
  then `Device programmed`.
- The standalone **read-only VERIFY** image booted. Two successive
  `SF5D STATUS` readouts agreed with the recorded SEED pre-update baseline:
  - `0x0C5000`: OTHER, first=`0x2578303D`,
    full-page CRC32=`0x8FCBBCDC` (**MATCH before/after**);
  - `0x0D5000`: OTHER, first=`0x09091701`,
    full-page CRC32=`0x1CAE7BAD` (**MATCH before/after**);
  - `0x0E4000`: MATCH, first=`0x53463544`,
    full-page CRC32=`0xAED32820` (**MATCH before/after**);
  - `matched=1/3 erased=0/3 seed_address=0x0E4000 seed_state=MATCH`.
- **SF5D3 sampled DFU preservation: PHYSICAL PASS.** These exact three
  4096-byte pages retained their full-page CRC32 values across this one
  successful **SEED → VERIFY** serial DFU update under installed bootloader
  handoff version `0.4.2`. Only the seed page had an intentionally written
  test marker; the other two contained existing unknown data.
- **Not proven:** 29 unsampled pages; a production-size image update;
  interrupted DFU; a future larger application, different bootloader or
  update path; or preservation of the adjacent protected partitions.
  **D1 remains OPEN; D2 remains OPEN.** No production observation/custody
  flash backend, firmware partition cutover or product ACK was activated.

**Full post-qualification regression (operator report, 2026-10-09):**
- `git pull --ff-only` on test branch from `5f517f3` to `574b6f9`
  (documentation-only evidence update).
- `bash firmware/tests/run_host_tests.sh > /tmp/orun-sf5d3-host.log 2>&1`
  returned **HOST_EXIT=0**, including production startup, history/security,
  GNSS/driver and tooling source-contract test paths in operator output.
- `pio run -e rak4630` **SUCCESS**, 82.732 s;
  RAM **29,536 / 248,832 B (11.9%)** and
  Flash **285,924 / 815,104 B (35.1%)**.
- Host PASS and production BUILD PASS do not mean production was physically
  uploaded or that the full 128 KiB bank is DFU-safe. RAK-1 was still
  running the test-only VERIFY image when this report was recorded.

**Revised experiment — smaller/safer bounded scope:**

Keep **three read-only observed pages**, each with a **whole-page CRC32**
and original first-word/status output:

- `0x0C5000` and `0x0D5000` are **read-only pre-existing controls**;
  do not write, erase, repair, or infer their previous ownership.
- `0x0E4000` is the **one and only writable sample**; `SF5D SEED CONFIRM`
  writes 16 marker bytes only if its entire 4096-byte page is erased.
  No erase primitive is linked to either experiment image.
- The test-only `rak4630_sf5d_dfu_verify` is read-only. Its ordinary serial
  DFU upload is the update under examination; it prints the same three
  whole-page CRC32 values and the final marker state.

**Operator procedure** (both images must be built again after this revision):

1. Rebuild `rak4630_sf5d_dfu_seed` and
   `rak4630_sf5d_dfu_verify`; run the focused source-contract guard.
2. Re-upload the revised `rak4630_sf5d_dfu_seed` to RAK-1 identified by its
   unique USB serial; open serial monitor and issue `SF5D STATUS`.
   **Record all three CRC32 values.** Do not continue if `0x0E4000` is OTHER.
3. If `0x0E4000` is `ERASED`, deliberately issue `SF5D SEED CONFIRM`
   and demand `SF5D DFU SEED PASS one-page-readback`, plus
   `seed_address=0x0E4000 seed_state=MATCH`. Record three CRC32 values
   after the marker is written, for a reliable **pre-update baseline**.
4. Close monitor with Ctrl+C, upload the separately built
   `rak4630_sf5d_dfu_verify` firmware over the SAME RAK-1 USB serial,
   reopen monitor and issue `SF5D STATUS`. Capture all three page CRC32s.
   Compare the `0x0C5000`, `0x0D5000` CRC32s before versus after and
   compare `0x0E4000` marker/readback/CRC32 before versus after.
5. Any mismatch is **evidence of changed data or a readout issue** requiring
   investigation before writes. Three matching CRCs plus a matching marker
   demonstrate retention of these particular sampled pages for this one
   tested update, not a general product guarantee.
6. Restore production firmware only as a **separate**, recorded DFU upload
   after the experiment; validate GNSS/LoRa independently.

**Limits:** This does NOT prove all 32 candidate pages survive, match the
installed bootloader binary to a reproducible 0.4.2 build, guarantee retained
data for larger firmware, test interrupted DFU, establish rollbacks or resolve
combined Tracker/Gateway partition capacity. **D1 stays OPEN**; no production
ObservationStore/CustodyStore activation follows from this test.

## 3. Gate D2 — owner and combined Tracker + Gateway, NOT YET CLOSED

**Role != Capability != Profile != Transport != Physical Flash Owner.**
Gateway bridging must not be modeled as an all-powerful legacy BASE role.
A single node may originate observations **and** forward foreign traffic.

The 128 KiB candidate is **one physical region, not two**. Proposed
restricted single-owner cases, subject to explicit product acceptance:

- Observation-only durable owner: ObservationStore has exactly the region.
- Custody-only durable owner: CustodyStore has exactly the region.
- Combined own-observation + foreign durable custody: **not resolved**.
  Both stores MUST NOT mount, erase, program or re-interpret the same page.
  Neither may promise an independent full 128 KiB guarantee from that shared
  area. Merely relaying/bridging data is **not** durable custody; it may remain
  possible, but the node MUST NOT emit a durable-custody ACK unless durable
  foreign-object ownership is actually established.

The combined product case requires an explicit capacity/ownership decision
before allocating flash: a fixed partition with stated per-purpose capacity,
a specifically reviewed shared durable format, a proven external resource, or
a deliberately bounded product mode without foreign custody. **None is
selected by this preflight.** Do not add a generic allocator or two
uncoordinated FlashMutationGate clients just to defer this decision.
With two hypothetical independent 128 KiB internal regions, a simple
equal-two-bank application arithmetic budget would be smaller than the
already measured application; that is a warning, **not** a universal proof
that every bootloader configuration is impossible.

**Owner transition ceremony:** a role/profile/capability/config change
must not implicitly format non-empty pages. Close/drain confirmed custody
responsibility first; unresolved Gateway custody blocks a destructive switch.
Tracker data re-baseline is allowed only after an explicit operator-visible
data-loss authorization. A power cut/reset mid-transition must never make
both writers claim the same physical region.

## 4. Gate D3 — future implementation sequence (not authorized here)

After D1 and D2 are evidenced/decided:
1. Define *one* explicit owner for each fixed page, with compile-time
   non-overlap checks and a post-link LOAD-address build guard. Keep all
   existing physical partitions and golden/TLP wire fixtures unchanged.
2. Add one bounded nRF observation/custody backend over the existing
   `FlashBackend` contract, **under the one existing FlashMutationGate /
   SoftDevice flash-token, event, and late-completion ownership model**.
   Do not instantiate an independently draining second event consumer.
3. Prove address bounds, word alignment, no overwrite/reprogram without erase,
   marker readback, scan time, power-loss/recovery and admission priority under
   deterministic host tests. No production owner cutover.
4. Run full host ASan/UBSan/-Werror tests and RAK4630 build/ceiling checks,
   independent focused review, then isolate physical qualification in SF5E
   on a **development device**. Firmware build is never a physical NVMC PASS.
5. Only after SF5E permits it, design the separately reviewed SF5F runtime
   tracker-data cutover and SF5G Gateway durable custody attachment.

### Explicit current disposition

**SF5C: MERGED portable-only. SF5D0: preflight documentation. SF5D
physical reservation/backend: NOT IMPLEMENTED. SF5E/SF5F: NOT STARTED.
Existing firmware and all physically proven M0–M5/R1–R4 paths remain untouched.**
