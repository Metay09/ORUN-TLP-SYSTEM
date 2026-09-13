# M4 flash journal (format 3, storage finalization R1.1)

## Partition ownership

`nrf52840_s140_v6.ld` ends application flash at `0xED000`. The installed
Adafruit core reserves seven 4,096-byte InternalFS pages from `0xED000` through
`0xF4000` (end exclusive). ORUN TLP owns this partition exclusively while the
journal backend is linked. InternalFS/LittleFS must not be mounted over it.

The build and runtime checks preserve the application, SoftDevice and
bootloader boundaries. A future filesystem or DFU design must repartition and
migrate storage explicitly before using these pages.

## Append-only nRF52 backend

`NrfHistoryFlash` uses Nordic's supported `sd_flash_write` and
`sd_flash_page_erase` APIs. It does not access NVMC registers directly. Program
operations are four-byte aligned, require erased destination bytes, and only
perform `1 -> 0` transitions. Writes cannot cross a physical page boundary.
Only SoftDevice-disabled storage is supported: the Nordic API completes
synchronously, and every operation result and physical readback is checked.
The backend checks SoftDevice state at initialization and before every program
and erase. Enabled SoftDevice or a failed state query returns failure before
issuing an operation. There is no asynchronous source buffer, semaphore,
completion callback or timeout path. M0-M5 has one main-loop storage owner and
never enables SoftDevice; concurrent enable/disable is not supported.

**SoftDevice-enabled persistent storage integration is an M7 prerequisite.**
M7 must design Bluefruit event routing and InternalFS partition ownership
together: standard `Bluefruit.begin()` calls `bond_init()` and mounts InternalFS.
Merely enabling Bluefruit is therefore incompatible with this exclusive raw
journal. Enabled-mode failure propagates through HistoryStore/PositionFlow and
suppresses POSITION TX without automatic reboot or infinite retry.

The Adafruit `flash_nrf5x` page cache is deliberately absent from the linked
image. Its flush path erases and rewrites a whole 4 KiB page for a small update
and does not return its lower-level erase/program status. That behavior cannot
provide journal append durability. The build rejects that cache path and
verifies that both Nordic primitives are linked.

## Layout and capacity

All seven pages are self-describing; no global metadata page is reserved.

| Item | Exact value |
| --- | ---: |
| Partition pages | 7 |
| Physical page | 4,096 bytes |
| Page header area | 352 bytes |
| Compact POSITION record | 36 bytes |
| Records/page | 104 |
| Maximum records | 728 |
| Maximum retention at 15 minutes | 7.5833 days |

The first 64 header bytes contain magic `ORJ4`, local storage format version 3, nonzero page
generation, device ID, CRC-32 and final commit. Eight 16-byte append-only
sequence-reservation slots follow. Four 32-byte delivery/replay-state slots
follow those; the final 32 header bytes remain erased (`0xFF`).

Each record is explicit big-endian serialization:

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
| 24 | 4 | High 32 bits of zero-based ticket `identity - 1` |
| 28 | 4 | CRC-32 of bytes 0..27 |
| 32 | 4 | Final commit word (`0`) |

Protocol version, packet type and device ID are reconstructed from page/device
context. Storing the high half of `identity - 1` makes identity reconstruction
exact at the `0xFFFFFFFF -> 0` wire-sequence boundary. Format 3 identifies this
corrected meaning explicitly; M2 POSITION wire bytes and protocol version are
unchanged. Old development v2 pages are never decoded as v3. If no valid v3
page exists and an ORJ4/v2 header is present, initialization fails without erase
or migration. Such a development board requires an explicit reset of ONLY the
ORUN partition before use; this firmware does not perform that reset. With
valid v3 pages present, v2/invalid pages are ignored and reclaimed only by the
normal circular lifecycle. There is no field data migration framework.

## Commit and power-loss semantics

CRC is CRC-32/ISO-HDLC (reflected polynomial `0xEDB88320`, init/final XOR
`0xFFFFFFFF`; `123456789` gives `0xCBF43926`). A blob's body and CRC are
programmed and read back before its final four-byte commit is programmed. The
complete blob is then read back. Both stages are append-only word programs.

A cut during a new record body or commit can discard that new record. It does
not erase or rewrite earlier committed records on the page. Torn headers,
records, reservations and state slots fail their CRC/commit checks and are
skipped during recovery.

During circular rotation, exactly the reclaimed physical page is erased once.
A cut during that erase can destroy the old contents of that reclaimed page;
the other six self-describing pages and their committed sequence reservations
remain recoverable. A cut during the new header or reservation leaves the old
active page intact. If no valid v3 page and no recognizable old v2 header exist,
the exclusive ORUN partition is treated as empty and page zero is reinitialized.
This includes bit-partial magic words, torn header CRC and torn header commit;
manual maintenance is not required after an interrupted first initialization.
If any valid v3 page survives, its records/reservations are preserved and no
blanket reset is performed. This policy assumes exclusive ORUN ownership, not
arbitrary foreign filesystem preservation. Loss of all valid headers through
unrelated corruption is outside the power-loss guarantee and can reset identity.

## Sequence and flow

TEST and POSITION share `SequenceSource`. Reservations contain 256 tickets.
Boot starts after the largest committed reservation and commits another block
before exposing a ticket, intentionally skipping unused tickets after reset.
When a running block is exhausted, `HistoryStore::poll()` commits the next
reservation automatically. Allocation requires an available reserved ticket;
append eligibility instead accepts an already allocated identity even when the
block has just been exhausted. Thus the final ticket can be committed as a
POSITION, and the next block's first ticket is unavailable until reservation
commit. A torn reservation therefore
cannot cause a previously exposed wire sequence to be reused.

`PositionFlow` remains store-first. A fresh fix is encoded, committed and read
back before one live TX attempt. Failure of page erase, page header,
reservation, record body, record commit or readback produces a terminal append
failure. `PositionFlow` leaves its pending state and does not transmit.

`TX_DONE` remains local transmitter completion, never BASE delivery. No current
caller advances delivered state from TX_DONE. Automatic backlog replay and BASE
confirmation remain future work.

## Flash operations and wear model

A normal record append performs two program operations (body/CRC and commit)
and zero page erases. Header, sequence reservation and replay-state updates use
the same two append-only program operations. A record-driven rotation performs
one page erase and six program operations: header pair, reservation pair and
record pair.

A physical page is reclaimed once per 7 × 104 = 728 normal records. At a
15-minute interval this is once per 7.583 days per physical page. Applying the
nRF52840 specification's 10,000-cycle value gives a nominal record-driven
arithmetic budget of about 75,833 days (207 years) per page. This is not a
physical endurance guarantee. Metadata-driven early rotations, temperature,
voltage, retention, manufacturing variation and actual workload still require
validation.

## Host and hardware validation

The host backend enforces page-granular erase, erased bits equal to one,
four-byte program alignment and `1 -> 0` programming. Fault injection covers
partial erase and partial body/commit programming. Tests cover virgin storage,
multiple same-page appends, exact erase/program counts, all record commit cut
points, page erase/header/reservation failures, header/reservation/record
readback failures, old-record survival, sequence non-reuse, circular wrap, CRC
corruption, identity boundaries, reservation exhaustion and PositionFlow
terminal failure. R1.1 additionally covers 255 TEST-style allocations followed
by last-ticket POSITION commit/reboot, next-reservation cuts, allocation →
append → reboot at all four uint32 boundary identities, bit-partial first
magic/CRC/commit, valid-page preservation and explicit v2 refusal.

A separate Linux host executable compiles the actual `nrf_history_flash.cpp`
against mocked Nordic APIs and memory-mapped test flash. It checks alignment,
page/partition bounds, status/readback failures, disabled-mode operations and
enabled-mode refusal before any API call, including terminal PositionFlow
failure. This models the supported synchronous contract; it does not validate
asynchronous SoftDevice integration or analog flash behavior.

Physical RAK4630 validation remains required for real power removal and
brownout at every operation stage, enabled-mode fail-safe refusal,
flash timing/current/endurance, repeated long-run rotation, firmware update
retention and concurrent LoRa/GNSS behavior.
