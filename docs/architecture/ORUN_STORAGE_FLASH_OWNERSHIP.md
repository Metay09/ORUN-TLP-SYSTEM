# ORUN Storage / Flash Ownership Audit

Status: **AUDIT — no runtime/protocol change**. This document is a prerequisite
gate for M7 BLE/DFU, future durable configuration/security material and
authenticated store-forward. It does not authorize any of that work by itself.

Audited main: `main@3002d060c79dec6cc0ed4bd6947c8cdf105a7b7c` (M6P1 closed;
`kTrackingIntervalSeconds = 3 * 60` development default).
Audit branch: `audit/storage-flash-ownership`.
Audit date: 2026-09-18.

Evidence sources actually opened for this audit:

- Repository: `AGENTS.md`; `docs/architecture/ORUN_CURRENT_ARCHITECTURE_RULES.md`;
  `docs/architecture/ORUN_SYSTEM_ARCHITECTURE_V1.md` (§9, §16, §19, §22);
  `docs/architecture/ORUN_ARCHITECTURE_GAP_ANALYSIS.md` (G06/G08); `docs/milestones/M4.md`;
  `docs/milestones/M6P1.md`; `docs/storage/M4_FLASH_JOURNAL.md`; `docs/audits/R1_STORAGE_FIX.md`.
- Code: `firmware/include/{storage_config,history_store,journal_format,flash_backend,nrf_history_flash,gnss_config}.h`,
  `firmware/src/{history_store,journal_format,nrf_history_flash,position_flow,main}.cpp`,
  `firmware/scripts/check_storage_layout.py`, `firmware/platformio.ini`,
  `firmware/rakwireless/boards/rak4630.json`.
- Installed PlatformIO packages (exact paths under
  `~/.platformio/packages/framework-arduinoadafruitnrf52` and
  `~/.platformio/platforms/nordicnrf52`), read directly, not assumed from the
  repository's prior description of them:
  - `cores/nRF5/linker/nrf52840_s140_v6.ld`
  - `libraries/InternalFileSytem/src/InternalFileSystem.cpp`
  - `libraries/InternalFileSytem/src/flash/flash_nrf5x.{c,h}`
  - `libraries/Bluefruit52Lib/src/bluefruit.cpp`, `utility/bonding.{h,cpp}`
  - `cores/nRF5/nordic/softdevice/s140_nrf52_6.1.1_API/include/nrf_soc.h`
  - `~/.platformio/platforms/nordicnrf52/builder/main.py` (upload command construction)
  - `~/.platformio/packages/tool-adafruit-nrfutil/site-packages/nordicsemi/...`
    (`__main__.py`, `dfu/dfu_transport_serial.py`)
- No RAK4630/RAK4631-specific bootloader binary or bootloader source exists in
  this repository or in the installed PlatformIO packages. Claims about the
  bootloader image itself are marked UNKNOWN below, not inferred from generic
  Adafruit-board bootloader conventions.

---

## 1. Scope

In scope: nRF52840 flash address ownership, the existing history journal's
invariants and power-cut model, the SoftDevice-disabled synchronous flash
backend, the InternalFS/BLE-bond conflict, DFU/bootloader knowns and unknowns,
storage prerequisites for a future authenticated ACK/store-forward design, and
capacity/wear arithmetic at the current development tracking interval.

Out of scope, explicitly not done here: any new partition allocation, any BLE
code, any config/security-material implementation, any protocol/ACK design,
any change to `firmware/include/gnss_config.h` or other runtime behavior, and
any change to TLP v1 bytes or golden fixtures.

## 2. Verified platform/build facts

All values below were read directly from the sources listed above, not
copied from an earlier document without re-verification.

| Fact | Value | Evidence |
| --- | --- | --- |
| MCU | nRF52840 (`NRF52840_XXAA`), 1 MiB flash, 256 KiB RAM | `firmware/rakwireless/boards/rak4630.json` (`extra_flags`, `mcu`), runtime `NRF_FICR->CODESIZE == 256` × `CODEPAGESIZE == 4096` = 1,048,576 bytes checked in `NrfHistoryFlash::begin()` |
| SoftDevice | S140 v6.1.1, FWID `0x00B6` | `rak4630.json` `build.softdevice` |
| Linker script | `nrf52840_s140_v6.ld` | `rak4630.json` `build.arduino.ldscript`; enforced by `check_storage_layout.py` |
| Application FLASH region (linker) | `ORIGIN = 0x26000, LENGTH = 0xED000 - 0x26000` (815,104 bytes) | `cores/nRF5/linker/nrf52840_s140_v6.ld`, line: `FLASH (rx) : ORIGIN = 0x26000, LENGTH = 0xED000 - 0x26000` |
| Board `maximum_size` | 815,104 bytes | `rak4630.json` `upload.maximum_size` — matches the linker length exactly (0xED000-0x26000 = 0xC7000 = 815,104) |
| InternalFS reservation | `LFS_FLASH_ADDR = 0xED000`, `LFS_FLASH_TOTAL_SIZE = 7*4096` | `InternalFileSystem.cpp`: `#ifdef NRF52840_XXAA #define LFS_FLASH_ADDR 0xED000 ... #define LFS_FLASH_TOTAL_SIZE (7*FLASH_NRF52_PAGE_SIZE)` (the `#else 0x6D000` branch does not apply on this MCU) |
| Physical page size | 4,096 bytes | `flash_nrf5x.h`: `#define FLASH_NRF52_PAGE_SIZE 4096` |
| `BOOTLOADER_ADDR` constant used by the core's own flash writer | `0xF4000` | `flash_nrf5x.c`: `#ifdef NRF52840_XXAA #define BOOTLOADER_ADDR 0xF4000` (guards `flash_nrf5x_write`'s destination bound, `VERIFY(dst < BOOTLOADER_ADDR, -1)`) |
| Bootloader settings address | `0xFF000` | `rak4630.json` `build.bootloader.settings_addr` |
| Upload protocol | `nrfutil` (serial DFU package upload) | `rak4630.json` `upload.protocol`; `firmware/platformio.ini` does not override it |
| Build-time layout guard | Fails the build if the linker origin/length string, `LFS_FLASH_ADDR`, `LFS_FLASH_TOTAL_SIZE`, `FLASH_NRF52_PAGE_SIZE` or `BOOTLOADER_ADDR` text differs from the audited values, and fails the link if the ELF contains an `InternalFS` symbol, a linked `flash_nrf5x_write`/`flash_nrf5x_flush` symbol, or is missing `sd_flash_write`/`sd_flash_page_erase` | `firmware/scripts/check_storage_layout.py`, wired as `pre:` and a post-link `AddPostAction` in `firmware/platformio.ini` |
| Runtime layout guard | `NrfHistoryFlash::begin()` requires `__flash_arduino_end == 0xED000`, `NRF_FICR->CODEPAGESIZE == 4096`, `NRF_FICR->CODESIZE == 256`, `NRF_UICR->NRFFW[0]` (bootloader address) either unset (`0xFFFFFFFF`) or `>= 0xED000 + 7*4096 (0xF4000)`, and SoftDevice disabled, before any read/program/erase is permitted | `firmware/src/nrf_history_flash.cpp` |
| `pio run -e rak4630` | SUCCESS, 0 warnings, RAM 14,156/248,832 (5.7%), Flash 147,144/815,104 (18.1%) | Re-run by this audit; unchanged from the M6P1 closure since this audit made no firmware change |
| Independent ELF verification | `arm-none-eabi-nm .pio/build/rak4630/firmware.elf` shows `000ed000 T __flash_arduino_end` (exactly `0xED000`); no `InternalFS` symbol; no `flash_nrf5x_write`/`flash_nrf5x_flush` symbol; `sd_flash_write`/`sd_flash_page_erase` both present | Run directly by this audit against the built ELF, independent of `check_storage_layout.py`'s own equivalent check, as an additional cross-check |
| `./firmware/tests/run_host_tests.sh` | **33/33 PASS**, exit 0, ASan/UBSan, `-Wall -Wextra -Werror`; includes `M4 storage repair regression checks: PASS` and `M4 production backend synchronous/SoftDevice guard checks: PASS` | Re-run by this audit; unchanged from the M6P1 closure since this audit made no firmware/test change |

## 3. nRF52840 flash ownership map (0x00000–0x100000)

All ranges are end-exclusive, matching the convention already used in
`ORUN_SYSTEM_ARCHITECTURE_V1.md` §9.

| Range | Size | Owner class | Evidence / certainty |
| --- | --- | --- | --- |
| `0x000000..0x026000` | 155,648 B (152 KiB) | MBR + SoftDevice S140 (combined reserved-below-application region) | **VERIFIED (boundary only)**: the linker's `FLASH` region starts at `0x26000`, so nothing below that address is application-linkable — proven by `nrf52840_s140_v6.ld`. The **internal split** between Nordic's MBR and the S140 SoftDevice image within `0x0..0x26000` is **UNKNOWN/FRAMEWORK-DEPENDENT**: no SoftDevice `.hex` or MBR image is bundled in this installed package (only S140 6.1.1/7.3.0 API *headers* exist, under `cores/nRF5/nordic/softdevice/`), so their exact sizes cannot be independently measured from this environment. Do not treat any sub-address in this range as free. |
| `0x026000..0x0ED000` | 815,104 B | Application firmware (ORUN TLP linked image) | **VERIFIED**: linker `ORIGIN/LENGTH`; matches `rak4630.json` `maximum_size` exactly. Unused linked capacity below the actual firmware size is firmware growth headroom, not an allocated data partition (per `ORUN_SYSTEM_ARCHITECTURE_V1.md` §9) — nothing in this audit changes that. |
| `0x0ED000..0x0F4000` | 28,672 B (7×4,096) | ORUN history journal — **exclusive** | **VERIFIED**: this is the exact core `InternalFileSystem` reservation (`LFS_FLASH_ADDR=0xED000`, `LFS_FLASH_TOTAL_SIZE=7*4096`) that ORUN's `NrfHistoryFlash`/`HistoryStore` uses directly as a raw, non-filesystem journal instead of letting `InternalFS` mount there. See §4. |
| `0x0F4000..0x0FF000` | 45,056 B | Bootloader image region (declared by the core's own `BOOTLOADER_ADDR` constant as the boundary above which the *application-linked* flash writer will not write) | **PARTIALLY VERIFIED**: `0xF4000` is proven as a **lower bound of a boundary the shipped Adafruit-style bootloader convention on this MCU respects** (it is the constant the core's own `flash_nrf5x.c` uses to refuse writes above). What is physically stored between `0xF4000` and the `0xFF000` settings page (bootloader code, padding, any additional metadata pages) is **UNKNOWN**: no RAK4630/RAK4631 bootloader binary or source is present in this repository or in the installed PlatformIO packages (the package only ships bootloaders for Adafruit's own boards — Feather nRF52840, PCA10056, Circuit Playground, etc. — not RAKwireless WisBlock). RAKwireless's actual flashed bootloader is an external artifact this audit cannot inspect. |
| `0x0FF000..0x100000` | 4,096 B (1 page) | Bootloader settings page | **VERIFIED (declared address only)**: `rak4630.json` declares `bootloader.settings_addr = 0xFF000`. This is a **board-metadata declaration consumed by PlatformIO's own tooling**, not a value this audit re-derived from a bootloader image (none is available). Whether an MBR parameter page exists immediately below it (a documented convention on some other Adafruit nRF52 boards) is **UNKNOWN/FRAMEWORK-DEPENDENT** for this specific RAK board and is not asserted here. |
| UICR (`0x10001000` region) / FICR (`0x10000000` region) | fixed SoC blocks outside the 1 MiB flash range | Separate factory/config registers, not general application storage | **VERIFIED as used today**: `NrfHistoryFlash::begin()` reads `NRF_FICR->CODEPAGESIZE`, `NRF_FICR->CODESIZE` and `NRF_UICR->NRFFW[0]` (bootloader start address per Nordic's UICR layout) purely as **read-only fail-closed guards**; ORUN does not write UICR and has no application storage there. |

Nothing above `0xF4000` is claimed free, reduced, or reallocatable by this
audit. The exclusivity claim in §4 is strictly `0xED000..0xF4000`.

## 4. Current application/history boundaries

- Application firmware: `0x026000..0x0ED000`, proven by the linker script and
  cross-checked against the board's declared `maximum_size`.
- History journal: `0x0ED000..0x0F4000`, proven to be **the exact same seven
  pages** the installed core's `InternalFileSystem`/LittleFS backend would use
  by default (`LFS_FLASH_ADDR`/`LFS_FLASH_TOTAL_SIZE`, same source file). ORUN
  does not carve out a new region; it takes over this existing reservation and
  uses it as a raw, non-filesystem, application-defined journal instead.
- `check_storage_layout.py` runs as a PlatformIO `pre:` script and fails the
  *build* if the installed linker script text, `LFS_FLASH_ADDR`,
  `LFS_FLASH_TOTAL_SIZE`, `FLASH_NRF52_PAGE_SIZE` or `BOOTLOADER_ADDR` no
  longer match the audited literal values (i.e., if a future PlatformIO/core
  package upgrade silently moved these addresses).
- The same script's `check_exclusive_owner` post-link action runs `arm-none-eabi-nm`
  on the built ELF and fails the *link* if: an `InternalFS` symbol is present
  (would mean LittleFS got linked in and could compete for the same region),
  a `flash_nrf5x_write`/`flash_nrf5x_flush` symbol is present (the Adafruit
  page-cache path, which erases and rewrites a whole 4 KiB page per small
  update and cannot provide the append-only durability the journal format
  assumes), or either `sd_flash_write`/`sd_flash_page_erase` Nordic primitive
  is *missing* (the append-only backend requires both).
- `NrfHistoryFlash::begin()` re-derives the same boundary at runtime from
  `__flash_arduino_end` (the linker-provided application end symbol) and two
  FICR fields, and refuses to become ready if any of them disagree with the
  audited constants, or if UICR's declared bootloader start address falls
  inside the history region, or if SoftDevice is enabled. This means a stale
  build (old ELF, new/different actual chip or bootloader) fails closed at
  runtime, not just at build time.

## 5. History journal invariants

Format: `journal_format` v3 (`kPageMagic = 0x4F524A34` "ORJ4", `kVersion = 3`).
Geometry: `storage_config.h` — `kPageSize=4096`, `kPageCount=7`,
`kPageHeaderSize=352`, `kRecordSize=36`, `kRecordsPerPage=(4096-352)/36=104`,
`kCapacity=7*104=728`. `static_assert`s in the same file enforce
`kBaseAddress + kRegionSize == 0xF4000` and exact page packing.

Each page's 352-byte header area is laid out as: a 64-byte static header
(magic/version/generation/device ID/CRC/commit), then 8×16-byte sequence
reservation slots (128 bytes), then 4×32-byte delivery/replay-state slots
(128 bytes); `64+128+128=320`, leaving the final 32 bytes of the header area
permanently erased (`0xFF`) — verified against `journal_format.h` constants
and `history_store.cpp`'s `sequenceOffset`/`stateOffset` helpers.

Verified invariants, by owner/topic:

- **Owner**: `HistoryStore` (portable format/state machine) +
  `NrfHistoryFlash` (concrete nRF52 program/erase/read backend implementing
  `FlashBackend`). `HistoryStore` never touches Nordic APIs directly.
- **Erase granularity**: one physical 4,096-byte page
  (`NrfHistoryFlash::erasePage`, `sd_flash_page_erase`).
- **Program granularity**: 4-byte aligned words only; `NrfHistoryFlash::program`
  rejects unaligned offset/size, verifies the destination is fully erased
  (`0xFF`) before writing, and reads back after `sd_flash_write` to confirm
  the physical result.
- **Page rotation**: round-robin by generation; `HistoryStore::startNewPage`
  reclaims the oldest page (erase, then re-header) once the active page is full.
- **Capacity**: 728 records (7×104), confirmed by `storage_config::kCapacity`.
- **Sequence reservation**: `TEST`/`POSITION` share one `SequenceSource`
  ticket space in 256-ticket blocks (`kSequenceBlockSize=256`); boot commits
  a fresh reservation block before exposing a ticket
  (`HistoryStore::begin()`: `next_ticket_ = sequence_end_; return startReservation();`),
  so a reboot never reuses a previously exposed wire sequence.
- **`delivered_through`/`replay_cursor`**: fields on `journal_format::State`
  (`journal_format.h`), persisted via the same commit-sealed state slots.
  **Verified: no current production caller advances either field.**
  `grep` across `firmware/src/*.cpp` and `firmware/include/*.h` finds
  `markDeliveredThrough`/`saveReplayCursor` defined only inside
  `history_store.cpp` itself; `main.cpp` and `position_flow.cpp` never call
  them. `TX_DONE` therefore cannot and does not advance delivery state today.
- **Power-cut commit ordering**: `HistoryStore::writeBlob()` — the single path
  used for *every* blob type (page header, sequence slot, state slot, record)
  — programs the body/CRC bytes first (`flash_.program(blob_offset_, blob_,
  blob_size_-4)`) and the final 4-byte commit word **last and separately**
  (`flash_.program(blob_offset_+blob_size_-4, blob_+blob_size_-4, 4)`), then
  reads back the whole blob. Each encoder's `seal()` (`journal_format.cpp`)
  writes a CRC-32/ISO-HDLC at a fixed offset and the literal commit word
  `kCommit = 0` at the last 4 bytes of that blob. Since NOR flash only allows
  `1→0` programming and erased bytes read as `0xFF`, an interrupted write
  before the commit word is programmed leaves that word at `0xFFFFFFFF`,
  which `sealed()` (`get32(p+commit)==kCommit`) correctly reports as
  **not committed**, regardless of whether the body bytes before it landed.
  This is the power-cut invariant, verified directly in source, not assumed.
- **CRC/commit semantics**: CRC-32/ISO-HDLC (`journal_format::crc32`,
  reflected polynomial `0xEDB88320`, init/final XOR `0xFFFFFFFF`); a slot is
  valid only if both its stored CRC matches the recomputed CRC over its
  payload bytes **and** its commit word equals `kCommit` (`sealed()`).
- **Corrupt record recovery**: `HistoryStore::recover()` reads every page's
  header, then every sequence slot until the first erased slot, then every
  state slot until the first erased slot, then every record slot (not
  stopping at the first erased record — it can skip a torn gap and continue).
  A failed `decodePage`/`decodeSequenceEnd`/`decodeState`/`decodeRecord` is
  counted in `diagnostics_.recovery_corruptions` and skipped; it does not
  abort recovery of the rest of that page or other pages.
- **Page generation**: newest `generation` value across all 7 pages selects
  `active_page_`; ties are not expected because generations are strictly
  increasing per rotation.
- **Overwrite behavior**: `startNewPage`'s erase step counts the reclaimed
  page's prior valid record count into `diagnostics_.overwritten` before
  erasing; only the single reclaimed page is erased per rotation.
- **Device identity binding**: every decode function takes `device_id_` and
  rejects records/pages/state encoded for a different device id
  (`decodePage`, `decodeRecord` via `validPacket`), so a partition physically
  moved between two device identities is not silently accepted (see
  `docs/storage/M4_FLASH_JOURNAL.md` for the exact v2/v3 identity-boundary
  history; not re-derived byte-by-byte in this audit beyond confirming the
  `device` parameter is threaded through as claimed).
- **Old-format handling**: `recover()` sets `old_format` when a page's magic
  matches but its version byte is `2`; if no valid v3 page exists anywhere
  and any v2-looking header was seen, `recover()` returns `false` (refuses to
  start) rather than auto-migrating or auto-erasing. This is a verified
  fail-closed path, not a claimed migration feature.
- **Boot recovery behavior**: if `active_page_ < 0` after `recover()` (truly
  empty/uninitialized partition, no old-format header seen either),
  `HistoryStore::begin()` calls `startNewPage(false)` to initialize page zero.
  If a valid v3 page exists, its records/reservations/state are kept exactly
  as recovered; no blanket reset occurs.
- **Wear implications**: see §12 for the exact record-driven arithmetic.

## 6. Power-cut / recovery model

Concrete cut-point behavior, verified from `writeBlob()`/`recover()`/`encodePage` above:

| Cut point | Result |
| --- | --- |
| Mid body/CRC program of a new record/header/reservation/state slot | Commit word for that slot never programmed; `sealed()` returns false; slot is skipped on recovery as if it never existed. Earlier committed slots on the same page are untouched (program only ever writes into previously-erased bytes at a new offset; it never rewrites an already-committed slot). |
| Mid commit-word program | Same as above — the commit word only ever transitions from `0xFFFFFFFF` (erased) to `0` in one 4-byte program; there is no intermediate "half-committed" value it can be read back as after a cut, because the read-back check inside `program()` itself would have already failed and reported false to the caller before the caller believed it succeeded. |
| Mid page erase (during rotation) | Only the single reclaimed page is affected; a cut during its erase can leave that one page's contents in an unspecified/partially-erased state, but the other six self-describing pages and their already-committed reservations/state remain independently recoverable, per `docs/storage/M4_FLASH_JOURNAL.md` and the structure of `recover()` (it evaluates each page independently). |
| Mid new-page header/reservation write after erase | The old active page (not yet reclaimed at that point in the state machine) remains intact; recovery simply does not find a newer generation and keeps using the prior active page. |
| Total loss of all 7 page headers (e.g., unrelated corruption of the whole region) | Outside the guaranteed recovery envelope; `recover()`'s `old_format`/empty-partition path would treat this as first-init and reinitialize page zero, which can reset sequence identity. This is stated as a documented limit, not a proven-safe recovery path, matching `docs/storage/M4_FLASH_JOURNAL.md`. |

What is **not** proven by host tests alone (explicitly not claimed here):
actual electrical brownout/power-removal timing on real hardware at each of
the above cut points, NOR wear/retention/temperature effects, and behavior
under a genuinely torn multi-word program on real silicon versus the host
fault-injection model. `docs/storage/M4_FLASH_JOURNAL.md` already states this
physical validation remains pending; this audit does not change that.

## 7. SoftDevice-disabled current model

`NrfHistoryFlash` uses only `sd_flash_write`/`sd_flash_page_erase`
(`nrf_soc.h`, `SD_FLASH_WRITE`/`SD_FLASH_PAGE_ERASE` SVCs). Every mutating
call is preceded by `synchronousFlashAvailable()`
(`nrf_history_flash.cpp`), which calls `sd_softdevice_is_enabled(&enabled)`
and requires both a successful query **and** `enabled == 0`. If SoftDevice is
enabled, or the state query itself fails, the operation is refused before any
Nordic flash API is invoked — verified directly in `program()`, `erasePage()`,
and `begin()`.

**Why synchronous-only works today**: with SoftDevice disabled, `sd_flash_write`/
`sd_flash_page_erase` execute and complete before returning `NRF_SUCCESS`, so
the existing code's immediate readback check is meaningful. This is stated
explicitly in `docs/storage/M4_FLASH_JOURNAL.md` and confirmed by the
`nrf_soc.h` API comments below.

## 8. M7 SoftDevice/BLE blocker

**Verified, not merely asserted**, by reading the actual SoftDevice API header
(`nrf_soc.h` in the installed `s140_nrf52_6.1.1_API`):

> `sd_flash_write`/`sd_flash_page_erase` doc comments: on success the command
> is only *started*; completion is signaled later via the SoftDevice event
> queue as `NRF_EVT_FLASH_OPERATION_SUCCESS` or `NRF_EVT_FLASH_OPERATION_ERROR`,
> and "the data in the `p_src` buffer should not be modified before" that
> event arrives **if the SoftDevice is enabled**.

This means an enabled-SoftDevice backend cannot reuse today's synchronous
call/readback pattern at all: it needs (a) an owned, stable source buffer kept
alive until the async event arrives, not a local stack blob copied inline as
`NrfHistoryFlash::program()` does today; (b) an explicit event-routing path
that dispatches `NRF_EVT_FLASH_OPERATION_SUCCESS`/`ERROR` back into
`HistoryStore`'s state machine instead of an immediate boolean return; and (c)
a defined behavior for what `HistoryStore`/`PositionFlow` do while a program/
erase is in flight (today's code assumes the call is already finished when it
returns).

**Can today's backend run with SoftDevice enabled?** **VERIFIED NO.**
`synchronousFlashAvailable()` returns false whenever SoftDevice is enabled, so
every `program()`/`erasePage()` call fails closed immediately; `HistoryStore`
then reports append failure and `PositionFlow` suppresses the live POSITION
TX (per `docs/milestones/M4.md`: "Storage failure suppresses normal POSITION
TX"). This is a safe failure, not a crash or silent corruption, but it means
**tracking effectively stops working the moment SoftDevice is enabled**, with
today's code, until an asynchronous backend is designed and swapped in.

**Can `HistoryStore`'s on-flash *format* be preserved while only the backend
changes?** Yes in principle — `HistoryStore`/`journal_format` only depend on
the `FlashBackend` interface (`read`/`program`/`erasePage`), not on the
synchronous timing of `NrfHistoryFlash`. A future asynchronous backend would
need to satisfy the same `FlashBackend` contract but complete calls via a
returned-pending/event-driven protocol instead of an immediate boolean; that
is a **new interface shape**, not a drop-in swap, and is explicitly not
designed here.

**Would BLE bonds/InternalFS compete with history for the same region?**
**VERIFIED YES — see §9.** This is the more urgent blocker: even before async
flash semantics are solved, simply calling `Bluefruit.begin()` corrupts the
history region outright (§9).

## 9. InternalFS conflict

**Verified from source, and stronger than previously documented.**
`AdafruitBluefruit::begin()` (`bluefruit.cpp`) calls `bond_init()`
unconditionally near the end of its startup sequence. `bond_init()`
(`utility/bonding.cpp`) calls `InternalFS.begin()` before creating its two
bond directories. `InternalFileSystem::begin()` itself
(`InternalFileSystem.cpp`):

```cpp
bool InternalFileSystem::begin(void) {
  // failed to mount, erase all sector then format and mount again
  if ( !Adafruit_LittleFS::begin() ) {
    for ( uint32_t addr = LFS_FLASH_ADDR; addr < LFS_FLASH_ADDR + LFS_FLASH_TOTAL_SIZE; addr += FLASH_NRF52_PAGE_SIZE )
      VERIFY( flash_nrf5x_erase(addr) );
    this->format();
    if ( !Adafruit_LittleFS::begin() ) return false;
  }
  return true;
}
```

Since ORUN's raw journal pages are not a valid LittleFS superblock, the first
`Adafruit_LittleFS::begin()` mount attempt over `0xED000..0xF4000` would fail,
and **`InternalFileSystem::begin()` would then erase all seven pages of that
exact region and reformat them as LittleFS, unconditionally, with no
confirmation step.** This is not a passive "cannot coexist" conflict; it is
an **active, automatic destruction of the entire history journal on the first
`Bluefruit.begin()` call**, verified directly in the installed core source.

`check_storage_layout.py`'s post-link `InternalFS` symbol check exists
specifically to catch this at build time: if any code path (including a
future accidental `Bluefruit.begin()`) links `InternalFS`, the build fails
before the binary can ever run on hardware.

**Explicit invariant**: *the history journal backend and `InternalFS`/LittleFS
must never be linked/active over the same pages at the same time.* Enabling
BLE with the stock Adafruit `Bluefruit.begin()` path, unmodified, is not a
future performance question — it is a data-destroying action against the
current partition. This audit does **not** assume "use InternalFS for future
config" as a solution; the partition/ownership decision must come first
(§15, decision gate B).

## 10. BLE bond/config/security ownership requirements

No code implements any of the following today. This table records ownership
*requirements*, not an allocation or a design.

| Data class | Durability | Write freq. | Power-cut sensitivity | Security sensitivity | Current owner | Future owner requirement | Reset behavior | Factory-reset behavior | Known flash region | Status |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| BLE bonds (LTK/IRK/peer data) | Durable across reboot | Low (per pairing) | Moderate (worst case: re-pair) | High (key material) | **None.** Stock `bond_init()`/`InternalFS` would run if `Bluefruit.begin()` were called as-is, and would destroy history (§9) | Dedicated partition, explicitly not InternalFS-over-history and not the journal's format | Clear on unpair | Must clear | **None allocated** | **BLOCKED** |
| Provisioned device/network configuration | Durable | Rare (on change) | Needs atomic old/new selection (per `ORUN_SYSTEM_ARCHITECTURE_V1.md` §9 CONFIG category) | Low–moderate | **None** — `ORUN_CURRENT_ARCHITECTURE_RULES.md` §6: "B4/M6 current configuration remains runtime-only. Do not allocate flash or claim durable configuration..." | Versioned config partition, atomic commit/migration | Explicit reset action (not implemented) | Reset to defaults | **Unallocated** | **UNALLOCATED** |
| Security keys/material | Durable, anti-rollback | Very rare (provisioning/rotation) | Critical | Critical | **None** | Dedicated protected partition; no shared erase policy with ordinary history | Not cleared by ordinary config reset | Explicit secure-erase/decommission path (not implemented) | **Unallocated** | **BLOCKED** (also blocked on: no cryptography/key scheme chosen — AGENTS.md: "Do not invent cryptography") |
| Anti-replay durable counters/state | Durable, monotonic, must never roll back | Per protected message/command (future) | Critical (rollback = security break) | High | **None** | Separate counter store, distinct from the cyclic history sequence reservation (`ORUN_SYSTEM_ARCHITECTURE_V1.md` §16: "separate counters from legacy wrapping uint32 sequence and ordinary history reclamation") | Must not reset on ordinary reboot | Explicit, documented rekey/counter-reset lifecycle only | **Unallocated** | **BLOCKED** |
| Ordinary user/application configuration (validated schema) | Durable | Occasional | Same atomicity needs as provisioned config | Low | **None** — RAM-only per B4 | Same as "provisioned configuration" row above; may share an allocator with it later, not with history | Reset to defaults | Reset to defaults | **Unallocated** | **UNALLOCATED** |
| History journal (position records) | Durable circular | Currently 480 records/day at the 3-minute development interval (§12) | **Known** — see §5/§6 | Location data, currently unencrypted at rest (v1 has no storage-at-rest cryptography; consistent with the documented v1 security limitation) | `HistoryStore`/`NrfHistoryFlash`, exclusively `0xED000..0xF4000` | Unchanged — already resolved | No explicit erase/factory-reset command exists in `main.cpp` today (verified: no "erase"/"factory" command string found) | **Not implemented** | `0xED000..0xF4000` | **KNOWN** |
| Delivery/backlog state (`delivered_through`, `replay_cursor`) | Durable (stored in journal state slots) | **Currently zero** — no production caller (§5) | Same commit-word-last pattern as the journal | Low | `HistoryStore` (fields exist, unused) | Authenticated BASE/server delivery-confirmation consumer (not designed) | Tied to journal partition | Tied to journal partition | `0xED000..0xF4000` (shares journal pages) | **KNOWN fields, UNUSED** |
| DFU/bootloader state (settings page, any MBR/bank metadata) | Durable, owned by the bootloader itself | Per DFU update | **UNKNOWN** — bootloader implementation not present in this environment | High (firmware integrity) | External bootloader (not this repository) | N/A for application code; application must never write `0xF4000..0x100000` | N/A | N/A | `0xF4000..0x100000` (boundary only, §3) | **KNOWN boundary / UNKNOWN internals** |

No row above allocates a page. "Unallocated" and "Blocked" are audit findings,
not proposals to place these in the history region or in InternalFS by default.

## 11. DFU/bootloader knowns and unknowns

**Known, verified from installed tooling:**

- Upload protocol is `nrfutil` (`rak4630.json`), which builds and transfers a
  `.zip` DFU package over the existing USB serial port using the bundled
  `tool-adafruit-nrfutil`'s `adafruit-nrfutil.py dfu serial` command
  (`~/.platformio/platforms/nordicnrf52/builder/main.py`).
- **The PlatformIO `nordicnrf52` platform's own upload-command construction
  unconditionally appends `--singlebank` to every `nrfutil dfu serial` upload
  for this board/protocol** (`builder/main.py`,
  `UPLOADERFLAGS=["dfu","serial","-p","$UPLOAD_PORT","-b","$UPLOAD_SPEED","--singlebank"]`).
  `nrfutil`'s own help text (`tool-adafruit-nrfutil/site-packages/nordicsemi/__main__.py`)
  documents `--singlebank`/`-sb` as: *"Single bank bootloader to skip firmware
  activating delay, default: Dual bank"*. **This means any "Single bank"
  wording seen in a `pio run -t upload` log is the PlatformIO/`nrfutil` upload
  *client* choosing single-bank transfer mode for this specific serial-DFU
  invocation — it is not evidence about what the physically flashed
  bootloader supports**, and it is not itself a claim about BLE DFU (a
  different transport is not proven to use the same flag).
- `dfu_transport_serial.py` shows the practical effect: with `--singlebank`,
  the tool skips the wait for the bootloader's "erase bank0 + copy bank1→bank0"
  activation step; with dual-bank (the nrfutil default, not what this board's
  build config passes), the new image would first land in a second bank and
  only then get copied into the running application bank.
- Application region boundary (`0x26000..0xED000`) and history exclusivity
  (`0xED000..0xF4000`) are unaffected by DFU either way; no DFU design here is
  authorized to encroach on the history pages (`AGENTS.md`, and unchanged by
  this audit).

**Explicitly UNKNOWN — must be verified physically, not assumed:**

- Whether the bootloader actually flashed on the owned RAK4630/RAK4631 units
  is dual-bank-capable at all, single-bank-only, or a RAKwireless-specific
  variant of the Adafruit bootloader. No RAK-specific bootloader binary or
  source exists in this repository or in the installed PlatformIO packages
  (only Adafruit's own board bootloaders — Feather/PCA10056/Circuit
  Playground/etc. — are bundled, none named for RAK4630/RAK4631/WisBlock).
- Whether single-bank serial upload as currently configured provides any
  rollback/fallback image if a serial upload is interrupted mid-transfer, or
  whether an interrupted single-bank update can leave the device unbootable
  until a J-Link/physical recovery. Given `--singlebank` explicitly skips the
  bank1→bank0 copy step nrfutil describes for dual-bank recovery timing, an
  interrupted single-bank transfer is **plausibly less recoverable than
  dual-bank**, but this is not proven against the real bootloader here.
- Whether the bootloader validates image signature/CRC before accepting a new
  application image, and what its exact interrupted-DFU recovery behavior is.
- The exact byte layout of `0xF4000..0xFF000` (bootloader image + any padding)
  and whether an MBR parameter page exists there, as seen on some other
  Adafruit nRF52 boards by convention — **not asserted for this board without
  a source or physical readout**.
- BLE DFU specifically (not yet implemented) may or may not reuse the same
  `--singlebank`/dual-bank choice; that is a future application-level
  decision, not something inherited automatically from the current serial
  upload configuration.

**Explicit non-assumption, per this audit's own constraint:** no part of this
audit proposes reclaiming any part of `0xED000..0xF4000` for a DFU bank, image
staging, or any other purpose. The history exclusivity invariant from §3/§4/§9
is unconditional here.

## 12. Store-forward/delivery-state implications

- `HistoryStore::getOldestUndelivered()`/`getNextBacklog()`/`backlogCount()`
  already exist and read from `state_.delivered_through`/`state_.replay_cursor`
  (`history_store.h`/`.cpp`), but **no production caller advances either
  field** (verified in §5). `TX_DONE` therefore cannot and does not act as
  delivery confirmation today, consistent with `AGENTS.md` ("TX_DONE is not
  delivery...") and `ORUN_CURRENT_ARCHITECTURE_RULES.md` §9.
- **Explicit forward statement, not implemented here**: `markDeliveredThrough()`
  must only ever be called in the future from a **trustworthy, authenticated**
  BASE/server delivery-confirmation path — never from local `TX_DONE`, never
  from an unauthenticated legacy v1 receive event.
- This audit does **not** design an ACK protocol, does not implement
  authentication, does not wire replay logic into production, and does not
  change record delete/erase semantics. Those remain future milestones.
- Prerequisite order for a future authenticated store-forward milestone, as a
  dependency list (not a schedule):
  1. Secure delivery envelope (authenticated, replay-resistant transport for
     acknowledgments) — depends on the security material ownership row in §10
     being resolved first.
  2. Authenticated ACK semantics (who may confirm delivery, and how a
     forged/duplicated ACK is rejected).
  3. Idempotent/deduplicated delivery confirmation (repeated ACKs for the same
     record must not double-advance `delivered_through`, and out-of-order
     ACKs must not regress it — `markDeliveredThrough()`'s existing
     `id < state_.delivered_through` guard already rejects regression at the
     storage layer, but the *decision* of which ACK is trustworthy is not yet
     designed).
  4. Backlog replay policy (rate, priority versus live data — `AGENTS.md`:
     "Live data must not wait behind a large historical backlog").
  5. Retention/overwrite priority once records may be "delivered" versus
     merely "aged out" by the circular buffer (today there is no distinction
     at all — the ring overwrites unconditionally at capacity regardless of
     `delivered_through`, since nothing consults it yet).

## 13. Capacity/wear arithmetic

Deterministic arithmetic only; **no battery-life or field-day claims** are
made anywhere in this section (per the earlier M6P1 closure, quantitative
power remains NOT MEASURED and is unrelated to this storage-only audit).

Constants: `kCapacity = 728` records, `kRecordsPerPage = 104`, `kPageCount = 7`.

| Interval | Records/day | Full-ring duration (728 records) | Specific-page reclaim interval (728 records touch that page once) | Any-page erase-event cadence (≈104 records) |
| --- | ---: | ---: | ---: | ---: |
| 15 minutes (900 s) — prior default, still documented in `M3.md`/`M4.md` as the historical reference point | 96 | 728/96 = **7.583 days** | 7.583 days | 104×900 s = 93,600 s ≈ **1.083 days (26 h)** |
| 3 minutes (180 s) — **current `main` development default** | 480 | 728/480 = **1.517 days (≈36.4 h)** | 1.517 days | 104×180 s = 18,720 s = **5.2 h** |

Applying the nRF52840 datasheet's 10,000-cycle nominal per-page erase budget
as a pure arithmetic ceiling (not a physical endurance guarantee, per
`docs/storage/M4_FLASH_JOURNAL.md`'s existing caveat, which this audit
repeats rather than strengthens):

- At 15 minutes: 7.583 days × 10,000 ≈ 75,833 days ≈ **207.6 years/page**
  (matches the figure already recorded in `M4_FLASH_JOURNAL.md`).
- At 3 minutes: 1.517 days × 10,000 ≈ 15,167 days ≈ **41.5 years/page** — five
  times more erase cycles consumed per unit time than the 15-minute figure,
  exactly proportional to the interval ratio. Still far beyond any realistic
  field deployment horizon as pure arithmetic; still not a measured endurance
  claim.

**Retention consequence worth flagging explicitly**: at the 3-minute
development default, the entire 728-record ring now covers only **~1.5 days**
of node-local history before the oldest record is overwritten, versus ~7.6
days at 15 minutes. Because §12 confirms no consumer currently reads
`delivered_through`/`replay_cursor` at all, this does not currently break
anything (nothing depends on longer retention today), but it substantially
shrinks the safety margin any future backlog-replay/delivery-confirmation
design will have to work within, and should inform the retention/overwrite
policy called for in §12, item 5.

This is a **single node-local** ring; this arithmetic says nothing about
multi-device RF domain capacity and does not assume any particular fleet size
(per the audit brief, no 1,000-device single-RF-domain assumption is made or
implied here).

## 14. Explicit UNKNOWN / physically-unverified list

- Internal MBR-versus-SoftDevice subdivision of `0x000000..0x026000` (§3).
- Exact byte contents/size of the bootloader image and any MBR parameter page
  within `0xF4000..0xFF000` (§3, §11) — no RAK4630/RAK4631 bootloader source
  or binary exists in this repository or the installed PlatformIO packages.
- Whether the physically flashed RAK4630/RAK4631 bootloader supports
  dual-bank DFU at all, independent of the `--singlebank` flag this build
  config currently passes (§11).
- Interrupted-DFU recovery behavior, image validation/signature checking, and
  rollback capability of the actual bootloader (§11).
- Electrical/physical brownout behavior at each journal power-cut point in
  §6, and NOR retention/wear under real temperature/voltage/workload — the
  host fault-injection model is not a physical proof (§6, consistent with
  `docs/storage/M4_FLASH_JOURNAL.md`).
- Real per-operation flash program/erase timing and current draw (out of
  scope for this audit; already NOT MEASURED for the M6P1 closure and
  unrelated instrumentation gap, not resolved here).
- Whether an MBR parameter page exists at all for this specific board/bootloader
  vendor combination (RAKwireless-flashed, not a stock Adafruit board) — not
  assumed from generic Adafruit nRF52 board convention.

## 15. Required decisions before M7 implementation

Items 1–3 below are now **design-decided** by
`docs/architecture/ADR_M7_PERSISTENCE_LAYOUT.md` (not yet implemented — see
that document's §16 implementation slices). Item 4 remains open and requires
physical hardware, not a design decision.

1. **Partition plan** — **DECIDED**: six new pages (`0x0E7000..0x0ED000`)
   carved from application headroom, strictly below history, for security+
   anti-replay (2 pages), config (2 pages) and relocated bonds (2 pages).
   None uses `InternalFS` over `0xED000..0xF4000` (§9 unchanged); none takes
   a history page. See the ADR §5–§7.
2. **Asynchronous flash backend design** — **DECIDED at contract level**: a
   single `FlashMutationGate`-style owner, bounded priority admission queue,
   per-store staging-buffer ownership, and an explicit "storage must
   complete before TX" invariant carried forward unchanged. See the ADR §9–§10.
   Not implemented; today's synchronous backend must keep failing closed
   under enabled SoftDevice until it is (`AGENTS.md`,
   `ORUN_CURRENT_ARCHITECTURE_RULES.md` §9).
3. **Bond storage backend choice** — **DECIDED**: relocate `InternalFS` via
   a targeted core patch (same idiom as `patch_wire.py`/`patch_radio.py`),
   not a from-scratch bond format and not a shared filesystem with config.
   See the ADR §8.
4. **Physical bootloader/DFU verification** (§11) — **still UNRESOLVED**:
   confirm on real hardware what the shipped bootloader actually supports
   before any BLE DFU design assumes dual-bank recovery or rollback. Not
   addressed by the ADR (decision gate F).

## 16. Required decisions before authenticated ACK/store-forward

1. Security key/material ownership and lifecycle (§10) — blocked on a chosen,
   reviewed cryptography approach; not designed by this audit.
2. Anti-replay durable counter ownership, explicitly separate from the
   cyclic history sequence space (§10, §12).
3. A defined "trustworthy delivery confirmation" semantic before
   `markDeliveredThrough()` gets its first real caller (§12).
4. A retention/overwrite policy that accounts for backlog replay once
   `delivered_through` is actually consulted by the circular buffer's
   reclaim logic (§12, §13) — today the ring reclaims unconditionally.
5. Backlog replay admission policy so historical replay cannot starve live
   data (`AGENTS.md`), once replay is implemented.

## 17. Decision gates (A–G)

Each answer is evidence-based per the sections above, not a judgment call.

| # | Question | Answer | Basis |
| --- | --- | --- | --- |
| A | Is the history region safe and exclusive today? | **VERIFIED YES** | Build guard (`check_storage_layout.py`) + runtime guard (`NrfHistoryFlash::begin()`) both independently enforce `0xED000..0xF4000` exclusivity and fail closed on any drift (§3, §4). No current code path links `InternalFS` alongside the journal backend. |
| B | Can `InternalFS` be used alongside history today? | **VERIFIED NO** | `InternalFileSystem::begin()` erases and reformats the entire region on a failed LittleFS mount, which the raw journal's non-LittleFS layout would always trigger (§9). The build itself refuses to link both (§4). |
| C | Can the current `NrfHistoryFlash` backend be used when SoftDevice is enabled? | **VERIFIED NO** | `synchronousFlashAvailable()` fails closed whenever SoftDevice is enabled; every mutating call is refused (§7, §8). This is a safe failure (POSITION TX suppressed), not silent corruption, but tracking storage stops functioning until an async backend exists. |
| D | Is there an assigned, safe persistent owner for BLE bonds/config/security material today? | **VERIFIED NO** | §10: every row is `BLOCKED` or `UNALLOCATED`; none has a flash region, and the one implementation that exists upstream (`bond_init()`) would destroy history if invoked unmodified (§9). |
| E | What storage/concurrency decisions are required before M7 BLE runtime starts? | **RESOLVED (design-level) by `docs/architecture/ADR_M7_PERSISTENCE_LAYOUT.md`** | The ADR decides the exact partition plan (6 new pages carved from application headroom below history), the bond backend (relocated `InternalFS` via a core patch, same idiom as `patch_wire.py`/`patch_radio.py`), the config/security backend (raw A/B pages, not LittleFS), the async SoftDevice flash contract shape, and concurrency/reset policy — none of it implemented yet; see the ADR's §16 implementation slices and §18 decision status. |
| F | What bootloader/flash facts must still be physically verified before BLE DFU starts? | **UNRESOLVED — see §11, §14** | Actual bootloader dual-bank capability, interrupted-DFU recovery, and image validation/rollback behavior are not provable from any source present in this repository or the installed toolchain; the currently-configured `--singlebank` serial upload flag is a client-side choice, not a bootloader capability proof. |
| G | What durable-state invariants are required before authenticated ACK/store-forward is connected? | **UNRESOLVED — see §12, §16** | Security key/counter ownership, a trustworthy delivery-confirmation semantic, and a retention/overwrite policy that actually consults `delivered_through` are all undesigned. The storage-layer guard against regressing `delivered_through` already exists (`markDeliveredThrough`'s `id < state_.delivered_through` check), but nothing decides which caller may legitimately advance it. |

## 18. Non-goals of this audit

This audit does **not**: implement BLE, DFU, durable configuration, security
material, or an ACK protocol; allocate any new flash partition; change
`firmware/include/gnss_config.h` or any other runtime constant; change TLP v1
bytes, RF parameters, relay semantics, or golden/compatibility fixtures;
change the journal's on-flash format or `FlashBackend` interface; or make any
quantitative power/current claim. It produces a canonical, source-verified
ownership map and an explicit decision/unknown list for the milestones that
follow.
