# M4 flash journal (format 2)

## Partition ownership

`nrf52840_s140_v6.ld` ends application flash at `0xED000`. The installed
Adafruit `InternalFileSystem.cpp` defines its InternalFS region as seven
4,096-byte pages beginning at that exact address. Therefore
`0xED000..0xF4000` (end exclusive) is the **core InternalFS partition**.

ORUN TLP currently owns that partition exclusively as its persistent journal.
It is not unallocated application flash. This firmware must not mount
InternalFS/LittleFS or another Bluefruit filesystem user at the same time. The
build checks the installed linker/core boundaries and rejects a linked
`InternalFS` symbol. Runtime checks also validate linker end, FICR page geometry
and bootloader boundary. If InternalFS is required later, the storage backend
must first be explicitly repartitioned and migrated; sharing this region is not
supported. Firmware, SoftDevice and bootloader space are never written.

## Core flash API and SoftDevice

`NrfHistoryFlash` uses the installed core's supported `flash_nrf5x_read`,
`flash_nrf5x_write`, `flash_nrf5x_flush`, and `flash_nrf5x_erase` primitives.
It does not manipulate NVMC registers directly and does not reject writes when
SoftDevice is enabled.

The audited core's `flash_nrf5x.c` invokes `sd_flash_write` and
`sd_flash_page_erase`. When SoftDevice is enabled it waits for the registered
`NRF_EVT_FLASH_OPERATION_SUCCESS`/error callback with its semaphore; when it
is disabled, the same calls complete synchronously. `program()` writes into the
core page cache, immediately calls `flash_nrf5x_flush()`, then reads back.
Thus a successful backend call is not merely a cache update. The core cache
flush rewrites a physical 4 KiB page, so an operation may block longer than the
small logical record. The journal has a durable commit boundary and never
claims measured timing or radio/IRQ isolation.

The SoftDevice-on compilation path is linked in the firmware build. BLE/actual
SoftDevice flash behavior has not been physically tested and remains pending.

## Layout and capacity

All seven pages are self-describing; no global metadata page is reserved.

| Item | Exact value |
| --- | ---: |
| Partition pages | 7 |
| Physical page | 4,096 bytes |
| Page header area | 352 bytes |
| Compact POSITION record | 36 bytes |
| Records/page | 104 |
| Usable records | 728 |
| 15-minute positions/day | 96 |
| Maximum retention | 728 / 96 = **7.5833 days** |

The first 64 header bytes contain `ORJ4`, format version 2, nonzero generation,
device ID, CRC-32 and final commit. The following 128 bytes contain eight
16-byte sequence-reservation slots. The following 128 bytes contain four
32-byte delivery/replay-state slots; the final 32 header bytes are reserved and
zero. A page is usable only after its static header commit validates.

Each 36-byte record is explicit big-endian serialization, never a C++ struct:

| Offset | Bytes | Value |
| ---: | ---: | --- |
| 0 | 4 | Original wire sequence |
| 4 | 4 | UTC epoch |
| 8 | 4 | Latitude E7 |
| 12 | 4 | Longitude E7 |
| 16 | 4 | Altitude mm |
| 20 | 2 | HDOP ×100 |
| 22 | 1 | Satellites |
| 23 | 1 | Flags |
| 24 | 4 | High 32 bits of journal identity |
| 28 | 4 | CRC-32 of bytes 0..27 |
| 32 | 4 | Final commit word (`0`) |

Protocol version, packet type and Device ID are fixed page/device context and
are reconstructed for replay. The original sequence plus high identity word
preserves both wire semantics and ordering across `uint32_t` wrap. A host test
reconstructs the full 34-byte POSITION packet and compares it byte-for-byte to
the original. The 34-byte M2 wire protocol is unchanged.

Current hardware/core partition capacity achieves at least seven days, but not
fourteen: 14 days require 1,344 records, while this safe partition contains
728. Fourteen-day retention needs a separately reviewed storage/layout or
hardware change; M4 does not consume firmware or bootloader space to claim it.

## Commit, recovery and circular behavior

CRC is CRC-32/ISO-HDLC (reflected polynomial `0xEDB88320`, init/final XOR
`0xFFFFFFFF`; `123456789` = `0xCBF43926`). A body/CRC is flushed and read back
before its final four-byte commit is flushed and read back. Torn headers,
records, reservation slots and state slots fail validation and are skipped.

Boot scans only this 28 KiB partition, finds valid page generations, record
ordering and valid reservations, and rebuilds RAM indexes. A torn compact body
or commit is rejected. However, the audited core cache flush physically
erase/rewrites its whole current 4 KiB page: a power cut during that flush can
also invalidate prior records in that active page. Other self-describing pages
remain independently recoverable. During circular wrap, the next physical page
is erased and receives its self-describing header; a power cut can lose that old
page but leaves the other six valid pages recoverable. A torn replacement header
is not treated as a valid page.

Virgin erased storage starts an empty journal automatically. For first
initialization, only an ORJ4 header may be partially programmed before any
record can exist. On reboot, erased storage or a recognizable torn ORJ4 prefix
is treated as empty and page initialization restarts automatically. Foreign
non-journal bytes are refused rather than erased. No ordinary initial power loss
requires manual maintenance.

## Sequence, delivery and flow

TEST and POSITION share `SequenceSource`. Every valid page has append-only
256-ticket reservations. Boot starts after the largest committed reservation
and commits a new block before exposing another ticket, so unused values are
intentionally skipped after reset/power loss rather than reused. Reservations
are in self-describing pages, not a single global metadata page. The persisted
identity high word avoids ordering ambiguity through wire-sequence wrap.

`PositionFlow` is strictly store-first: fresh valid fix → M2 encode → journal
commit/readback → one live TX attempt. Commit failure suppresses normal POSITION
TX. TX_DONE is only local transmitter completion: it does not delete records or
advance delivered state. There is no BASE confirmation, ACK protocol, automatic
backlog replay or inferred delivery in M4. The persisted cursor APIs remain a
future transport seam and are never invoked by TX_DONE.

## Validation limits

Host tests cover erased boot, every first-init program-cut boundary, torn page
header/erase, torn compact record, compact semantic reconstruction, corruption,
page wrap, circular overwrite, reboot recovery, reservation interruption,
sequence wrap encoding, cursor persistence and no delivery mutation after live
TX. The memory backend cannot reproduce analog partial full-page cache-flush
behavior; that physical fault case remains pending validation.
Physical validation remains required for power removal/brownout, actual flash
timing/endurance/current, SoftDevice/BLE operation, and LoRa/GNSS behavior while
the core flash primitive is active.
