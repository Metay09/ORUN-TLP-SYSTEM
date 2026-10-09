# SF5D — nRF52840 Flash Ownership + DFU Preflight

Status: **SF5D0 PREFLIGHT IN PROGRESS — DFU PRESERVATION GATE BLOCKED PENDING INSTALLED BOOTLOADER EVIDENCE; DESIGN/READ-ONLY ONLY.** No physical
128 KiB reservation, no `FlashBackend` added, no change to the production
linker/build ceiling, no ObservationStore/CustodyStore runtime attachment,
no erase/program and no SF5D/SF5E physical PASS.

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

The two current RAK4631 units are development devices; the owner accepts
loss of **existing test records** for deliberate qualification. This does
**not** relax the future product requirement that updates preserve durable
user observations. It is also not permission to erase bootloader/SoftDevice,
change production application boundaries or activate the unallocated
128 KiB region. Gate D1 must be closed with observed update/erase behavior
before any production storage writes.

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
