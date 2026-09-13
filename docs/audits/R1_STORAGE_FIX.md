# Storage repair R1 / R1.1 finalization

## Scope

R1 fixes post-M5 audit findings C1, H1, H5, M1 and M2 only. Radio, GNSS,
relay timing, BLE, accelerometer and M6 behavior are unchanged.

## Root cause and repair

The former production backend sent each small journal write through Adafruit's
`flash_nrf5x` cache. Every flush erased and rewrote the complete 4 KiB page,
which exposed earlier committed records and sequence reservations to every
later append. The cache also discarded the underlying erase/program result.

R1 programs aligned words through Nordic `sd_flash_write`, using only `1 -> 0`
transitions. `sd_flash_page_erase` is used only when the journal deliberately
initializes or reclaims a page. SoftDevice-disabled operations complete
synchronously. Return status and physical readback are checked. R1.1 removes
the incomplete asynchronous semaphore/callback/timeout implementation: enabled
SoftDevice or a failed state query is rejected before every program/erase API
call, including after successful initialization. There is no asynchronous stack
buffer lifetime and no automatic reboot/retry loop.

Local journal format is now **v3**. It stores the high half of the zero-based
ticket (`identity - 1`) and rebuilds the full ticket before adding one. V2-only
development storage is explicitly unsupported without modification or erase;
an intentional development reset of the exclusive ORUN partition is required.
V2 pages are never silently interpreted as v3. No migration framework is added.
The M2 wire protocol is unchanged.

Sequence block exhaustion now starts a new durable 256-ticket reservation from
`HistoryStore::poll()`. The next block remains unavailable until its reservation
commit succeeds. R1.1 separates new-ticket eligibility from append eligibility,
so the already allocated last ticket can actually be persisted as POSITION.
Boot still skips the
largest committed reservation, preventing exposed sequence reuse after reset
or a torn later reservation.

Any erase, header, reservation, record, commit or readback failure belonging to
a pending append now publishes a failed append result. `PositionFlow` consumes
that result, leaves `appending_`, retains store-before-TX semantics and sends no
POSITION.

## Transaction and wear model

Normal record append:

- 2 program operations: body/CRC, then commit
- 0 page erases

Record-driven page rotation:

- 1 erase of the reclaimed page
- 6 program operations: header pair, reservation pair, record pair

A physical page is reclaimed once per 728 normal records. At a 15-minute
interval, the nominal interval is 7.583 days per erase of a given page. Applying
the device specification's 10,000-cycle value yields about 75,833 days (207
years) of record-driven arithmetic budget. This excludes early metadata-driven
rotation and is not a physical endurance guarantee.

## Power-loss guarantee

Body and CRC are written and verified before a final commit word. A cut during
the new body or commit makes only the new blob invalid. Earlier data is never
erased or rewritten by a normal append. Rotation can sacrifice only the page
being reclaimed; the other six pages retain valid records and sequence
reservations.

R1.1 also recovers bit-partial first-page magic, CRC and commit. If there is no
valid v3 page (and no old v2 header), initialization resets page zero in the
exclusive ORUN partition. Surviving valid pages prevent empty reinitialization.
This is not a recovery promise for unrelated destruction of every page header.

## M7 prerequisite

SoftDevice-enabled persistent storage integration is an M7 prerequisite.
Bluefruit event routing and InternalFS ownership must be designed together;
standard Bluefruit startup mounts InternalFS over the same partition. M0-M5
supports only the synchronous SoftDevice-disabled mode. No M7 BLE work is added.

## Regression coverage

The host flash backend models page-granular erase, erased bits equal to one,
aligned `1 -> 0` programming, partial program failure and partial erase. Tests
cover virgin initialization, same-page appends, exact wear counts, every record
body/commit cut point, page transition erase/header/reservation/record failures,
header/reservation/record readback failures, old-record survival, sequence
non-reuse, circular wrap, corrupt CRC,
`0xFFFFFFFE`, `0xFFFFFFFF`, `0x100000000` and `0x100000001` identity cases,
reservation exhaustion, uint32 wire wrap and PositionFlow failure completion.
R1.1 tests all 256 tickets, last-ticket POSITION persistence and reboot, next
reservation body/commit cuts, allocation/append/reboot around uint32 wrap,
bit-partial magic/CRC/commit and v2 refusal. A separate executable compiles the
production nRF backend with mocked Nordic APIs to verify synchronous operation,
alignment/bounds/readback and enabled-state rejection before flash starts.

The complete M3/M4/M5 host runner passes with ASan, UBSan and `-Werror`.
The RAK4630 PlatformIO build succeeds at 12,468 bytes RAM (5.0%) and 132,608
bytes flash (16.3%).

## Physical validation pending

RAK4630 hardware must still validate real power removal and brownout during
each program/erase phase, SoftDevice-enabled fail-safe refusal,
long-run flash rotation, timing/current/endurance, firmware-update retention and
LoRa/GNSS behavior during flash operations.
