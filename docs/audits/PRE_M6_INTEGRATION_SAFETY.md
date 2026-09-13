# Pre-M6 startup / storage identity safety

Reviewed base: `9ea359d8c67fe81c7c54d77e5db9e8f84a250ad6`.
Branch: `fix/pre-m6-integration-safety`.

## Finding: CONFIRMED

The production cold-boot path allowed real history loss:

1. `RadioManager::begin()` could return false from driver mutex initialization
   or gate acquisition before calling `BoardGetUniqueId()`. A newly constructed
   manager therefore still returned its default device ID, zero.
2. `setup()` ignored this result and called `history.begin(0)`.
3. `HistoryStore::recover()` used `decodePage()` with that ID. Valid v3 headers
   for the actual nonzero device ID failed the identity comparison, leaving
   `active_page_ == -1`. The v2-only protection does not protect v3 pages here.
4. `begin()` scheduled `startNewPage(false)`: target page 0, phase `kErase`.
5. The next normal `loop()` called `history.poll()` because no TX was active.
   `NrfHistoryFlash::erasePage(0)` passed the valid partition and disabled
   SoftDevice checks and issued `sd_flash_page_erase()` for that physical page.

This is physical page loss, not just a temporarily empty recovered view.
Other pages are initially skipped, not immediately erased. A lost header and
reservation can also reset the recovered sequence range.

Before changing production code, host fault injection ran the actual setup,
loop, HistoryStore and nRF backend with a committed record on page 0. A mutex
creation failure produced ID zero and one page erase on the first loop; the
partition bytes changed. The new identity regression assertion also failed
against those pre-fix sources.

## Minimal fix

Capture the board ID and bind the sequence source at the beginning of
`RadioManager::begin()`, before every radio failure return. The pinned nRF52
`BoardGetUniqueId()` implementation reads factory identity registers at
`0x10000060` and `0x10000064`; it does not require radio initialization or a
driver lock. Byte ordering and existing device IDs are unchanged.

Binding the sequence source early also preserves local POSITION encoding and
store-first append when the radio is unavailable. Leaving it after the gate
would preserve recovered history but still prevent new local GNSS records.

`setup()` explicitly checks the radio result and logs failure, then continues
storage recovery and GNSS initialization. Existing readiness checks keep TX/RX
disabled after startup failure, while the cooperative loop services storage,
GNSS, watchdog feeding and idle.

No storage guard or format change is needed for this path: every return from
radio begin now has the hardware identity, rather than an uninitialized zero.
HistoryStore's existing explicit-ID API and M4 recovery/rotation semantics are
unchanged. No new reserved ID, migration or automatic erase policy is added.

## Regression coverage

`firmware/tests/startup/test_startup.cpp` includes production `main.cpp` and
links the actual radio manager/gate, GNSS state machine, PositionFlow,
HistoryStore, journal codec and nRF flash backend. It reuses the existing host
stubs and fixed-address flash mapping; no new test framework is introduced.
Only hardware/RTOS interfaces and the watchdog boundary are substituted.

Five separate cold-boot processes cover mutex creation failure, gate failure,
event queue failure, LoRa initialization failure and successful startup. Each
checks hardware identity, conditional main failure diagnostics, recovered
record bytes, zero erases through recovery, and reservation-only programming.
Matched fresh PVT/DOP callbacks then exercise GNSS -> local append with correct
identity and a non-reused sequence. Failed startup sends/receives nothing;
successful startup enters RX and transmits the newly committed POSITION.
Watchdog and idle counters prove that main continues completing its loop.

## Local validation

- `firmware/tests/run_host_tests.sh`: PASS, including M3/R3, R1/M4 storage,
  production nRF backend, M5, R2, all five startup scenarios and R4.
  Host compilation retains `-Wall -Wextra -Werror` and ASan/UBSan.
- `cd firmware && pio run`: SUCCESS, RAK4630 target. RAM: 13,844 / 248,832
  bytes (5.6%); flash: 139,608 / 815,104 bytes (17.1%).
- `git diff --check`: PASS; changes reviewed on the fix branch.

## Physical validation

**Not performed.** Host API fault injection and compilation do not prove
physical flash or RF behavior. On RAK4630, inject a radio startup failure with
existing journal records and verify the boot diagnostic, preserved records,
continued local GNSS appends and watchdog servicing, then repeat normal startup.
