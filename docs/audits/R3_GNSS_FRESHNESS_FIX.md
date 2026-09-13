# R3 — GNSS freshness and same-PVT UTC

Scope: `fix/r3-gnss-freshness`, base `36ed21c`. No commit or push. R3 changes
GNSS acceptance, UTC conversion, boot detection and local live-position age.
The 34-byte POSITION layout, version 1, journal format, R1 storage ownership,
R2 radio ownership and M5 topology remain unchanged. No hardware validation
is claimed.

## Verified call flow and original failures

The production path is:

1. `loop()` calls `GnssManager::poll()`.
2. In ACQUIRING, `checkUblox()` parses UBX messages, then `checkCallbacks()`
   dispatches NAV-DOP and NAV-PVT callback copies synchronously on the loop task.
3. `onPvt` / `onDop` call `handlePvt` / `handleDop`, which keep one candidate
   of each type and call `considerPositionFix()`.
4. Matching promotes a separate single-use `GnssFix` and enters FIX_AVAILABLE.
5. The main loop services the journal and pending PositionFlow, then calls
   `takeFreshFixForTransmission()` when TRACKER and storage can accept a fix.
6. `PositionFlow::acceptFix()` calls the production POSITION encoder and
   `HistoryStore::append()`. Subsequent `history.poll()` commits the record.
7. `PositionFlow::update()` takes the append result and attempts live TX only
   after successful persistence. `RadioManager::sendPositionPacket()` checks
   radio availability under the R2 driver gate and calls `Radio.Send()`.

At the base revision, candidate storage had iTOW and payload but no arrival
times. `considerPositionFix()` required matching iTOW, then assigned
`fresh_fix_at_ms_ = now`. A PVT captured at t0 and same-iTOW DOP received more
than five seconds later therefore produced a new five-second fresh lifetime.
The reverse order had the same defect. Matching an epoch proves association,
not age. PositionFlow independently reset its five-second lifetime using
`accepted_at_ = now`, extending the apparent age again after manager handoff.

The UTC defect is independently confirmed in installed SparkFun **2.2.29**:

- `processUBXpacket`, NAV-PVT branch, updates `packetUBXNAVPVT->data` on each
  parsed PVT. It copies to `callbackData` only when `callbackCopyValid` is false
  (source lines 3396–3401). NAV-DOP uses the same one-copy pattern (3307–3312).
- `checkCallbacks()` passes `callbackData` to the pointer callback, then clears
  its valid flag. DOP is dispatched before PVT (5372–5388, 5410–5426).
- `getUnixEpoch(uint16_t)` (17433–17460) takes no snapshot argument. It may call
  `getPVT(maxWait)` when the seconds getter flag is stale, then converts fields
  from **`packetUBXNAVPVT->data`**, not callbackData. Passing zero does not bind
  the getter to the callback. PVT A's callback can thus combine A's coordinates
  with PVT B's cached UTC. The old lookup-table conversion also does not validate
  the application's wire calendar bounds.

These line numbers refer to the pinned dependency's
`src/SparkFun_u-blox_GNSS_Arduino_Library.cpp`, installed under
`firmware/.pio/libdeps/rak4630/SparkFun u-blox GNSS Arduino Library/`.
[Upstream v2.2.29 source](https://github.com/sparkfun/SparkFun_u-blox_GNSS_Arduino_Library/blob/v2.2.29/src/SparkFun_u-blox_GNSS_Arduino_Library.cpp)
contains the inspected implementation. The dependency is not patched.

## Candidate age and session model

PVT stores iTOW, copied coordinates/flags/satellites, snapshot-derived UTC,
`GnssFix::captured_at_ms`, and `pvt_generation_`. DOP stores iTOW, hDOP,
`dop_received_at_ms_`, and `dop_generation_`. Capture time is sampled at callback
entry with the existing M3 `monotonic::nowMs()`. No raw millis freshness clock,
heap allocation, String, UTC-based scheduling or new wire timestamp is added.

Promotion requires both candidates, both generations equal to the active
session, equal iTOW, the existing PVT validity/range checks, an active acquisition
before its timeout, and **each candidate age strictly less than 5000 ms**.
The threshold is the existing central `gnss_config::kFreshFixMaxAgeMs`; equality
at 5000 ms is expired. No second arrival-skew threshold is necessary. DOP has
no independent validity bit in NAV-DOP; the existing parsed-message/presence
semantics and hDOP units are retained without inventing a quality cutoff.

Promotion copies the PVT timestamp unchanged. Repeated valid callbacks for the
currently retained candidate iTOW do not overwrite its capture time. A stale
pair remains ineligible; observing a different epoch can replace candidates.
Invalid PVT still invalidates the candidate. The last-promoted-epoch and
single-consumption guards are retained.

Each `startAcquisition()` increments a uint32 session generation and clears
candidates, boundary epochs and any unconsumed old fix. Timeout and configuration
failure clear candidates and close acceptance; success closes acceptance;
power-down/idle entry clears candidates. Callbacks outside ACQUIRING are ignored.
`begin()` resets local state before the boot power/detection sequence.

Generation alone cannot identify the creation session of unparsed UBX bytes:
the receiver does not carry an application generation. The transport boundary
therefore matters as well:

- STARTING drains library callback copies with acceptance disabled.
- The 2.2.29 I2C reader can return true without reading when its polling wait
  has not elapsed. R3 temporarily sets the public polling-wait API to zero for
  the drain, then restores the effective existing 100 ms poll policy for 1 Hz
  navigation. It requires a successful available-byte-batch read. Empty reads
  and transport failures stay in STARTING, bounded by the acquisition timeout.
  Retrying the drain uses the same 100 ms monotonic interval, so forcing the
  library read cannot create an every-loop I2C retry storm.
- The reader consumes the announced byte batch synchronously; no independent
  callback worker can deliver an application candidate later. A byte batch may
  end in a partial UBX frame. The first post-drain PVT **and DOP** epochs are
  boundaries and cannot be accepted. A changed epoch of each type is required.
  R3 extends M3's PVT-only boundary to DOP. This can conservatively cost another
  navigation epoch when the streams are not aligned.

Together, drain, state gating, per-type boundaries, candidate generation and
age reject prior-session candidates and pending/partial transport output under
the inspected synchronous ordered transport. GPS-week wrap uses equality,
not numeric ordering: 604799000 to 0 is legal for new candidates. A later
session can reuse an iTOW, but cannot pair with the earlier session's candidate.
Generation wrap needs no global epoch registry: candidate lifetimes never
span a full generation cycle, and every acquisition clears them.

## UTC snapshot and deterministic conversion

In `handlePvt`, the callback's `year`, `month`, `day`, `hour`, `min`, `sec` are
copied into a small stack `UtcSnapshot` and converted immediately. The resulting
epoch is stored with that PVT's coordinates. Fresh POSITION creation no longer
calls `getUnixEpoch()` or any other mutable GNSS cache getter.

The exact 2.2.29 fields and bit names were checked in `src/u-blox_structs.h`,
lines 364–383: `valid.bits.validDate`, `valid.bits.validTime`, and
`valid.bits.fullyResolved`. Both existing date/time bits remain necessary;
fullyResolved is **not** newly required. Invalid UTC does not reject otherwise
valid coordinates: epoch stays zero and UTC-valid stays clear.

`utcToEpoch()` validates Gregorian month lengths, leap years including century
rules, day, hour 0–23, minute 0–59 and second 0–60. It uses integer arithmetic,
small constant storage and bounded loops (at most 136 years and 11 months),
without timezone, locale, time_t, heap or OS date APIs. uint64 arithmetic checks
the exact uint32 wire range: 1970-01-01 00:00:00 through
2106-02-07 06:28:15. Epoch zero at the lower bound can have UTC-valid set;
the flag distinguishes it from invalid time.

NAV-PVT explicitly permits `sec=60` in the
[u-blox M8 protocol specification, NAV-PVT](https://content.u-blox.com/sites/default/files/products/documents/u-blox8-M8_ReceiverDescrProtSpec_UBX-13003221.pdf)
(PDF page 390, printed page 375, section 32.17.17), also reflected in the pinned
struct. R3 retains the previous integer-second normalization: second 60 maps
to the following minute. It does not require a leap-second calendar or newly
reject the documented value. Fractional `nano` is not applied, matching the
previous integer-second getter path. This wire format cannot distinguish the
leap second from the adjacent normalized Unix second.

## Capture-to-live age and persistence

`GnssFix::captured_at_ms` survives manager promotion and consumption. PositionFlow
rejects an already-expired handoff and retains the capture timestamp separately
from the encoded packet while append or radio availability is pending.

Once append begins, it completes using the existing journal state machine even
if the fix expires meanwhile. Storage failure prevents live TX. Successful
commit followed by age >=5000 ms emits `kLiveExpired`, removes only the volatile
live candidate, and keeps the committed record and original UTC/flags/sequence
as historical backlog. Delivery state is not changed. No monotonic timestamp
is written to flash or restored across reboot.

Before live admission, PositionFlow checks capture age and passes its local
timestamp into RadioManager. RadioManager rechecks the actual monotonic clock
under the driver gate before starting the TX operation, covering time spent
between the caller's sampled time and radio admission. The TX Serial log now
runs after `Radio.Send()` so logging cannot extend the age before admission.
Gate contention remains a defer, not delivery or an attempted transmission.
TX_DONE still does **not** mean DELIVERED.

Freshness is a sender admission policy, not a bound on LoRa airtime, relay delay
or receipt time. The stored-packet radio API still permits a caller without a
live timestamp; all production PositionFlow live calls supply it. Automatic
historical replay and BASE delivery confirmation remain unimplemented as in
M4/M5. R3 does not enable replay or relabel a historical record as a live fix.

## Transient detection and retained M3 behavior

The base code entered permanent NOT_PRESENT on the first unsuccessful library
`begin()`. Main then resolved AUTO to BASE once. R3 makes at most **three boot
detection calls**, waiting **5 seconds then 10 seconds** after failures, using
the monotonic clock. Each call retains the existing 250 ms library maxWait;
2.2.29 `begin()` itself tries `isConnected()` up to three times. There is one
call per cooperative detecting pass, no tight retry loop or new bus recovery.

The rail stays powered during these two bounded backoffs. Exhaustion switches
WB_IO2 low and stays NOT_PRESENT until reboot/explicit local reinitialization.
There is no periodic hot-plug probe. Detection success starts the first
acquisition immediately and anchors its schedule at success. `detectionComplete()`
is false during backoff, so main's existing AUTO resolution waits for success
or final absence. Its initial BASE radio availability continues meanwhile.
A successful retry resolves AUTO to TRACKER; final absence resolves BASE.
Explicit role overrides still win, and profiles do not disable GNSS hardware.

Unchanged: default 15-minute grid, 120-second timeout including configuration,
no overlapping acquisition/catch-up burst, <=60-second continuous tracking,
<=2-second next-due power-cycle avoidance, normal WB_IO2 switched-slot policy,
MCU idle and TTFF measured through successful PVT/DOP acceptance. RTC/backup
retention and hot starts are not assumed. The RAK12500/ZOE-M8Q configuration
remains UBX over Wire at 0x42, 1 Hz navigation, continuous receiver mode and
automatic PVT/DOP, with volatile reconfiguration after timeout/power cycles.

Added fixed-size diagnostic counters: stale PVT rejection, stale DOP rejection,
invalid flagged UTC calendar and detection retries. Existing expiry and flow
events remain available. No per-poll logging or dynamic diagnostics are added.

## Regression coverage and validation

Tests compile production GnssManager/UTC code, PositionFlow/journal/encoder, and
RadioManager in their existing host harnesses. The callback stub models the
inspected separate first-unconsumed callback copy and mutable current PVT cache;
it does not claim to emulate the full UBX parser or physical I2C.

| Required scenario | Deterministic coverage |
| --- | --- |
| A, I: normal pair, both orders, single-use | M3 `testFixes`, R3 `ageIsNotRenewed` |
| B, C: delayed same-iTOW PVT/DOP, exact expiry and clock wrap | R3 `test_delayed_pair.cpp` |
| D: match/duplicate does not renew age, expiry after match | R3 `ageIsNotRenewed`, `repeatedStaleEpoch` |
| E, F: timeout/sleep/restart, queued callback, same iTOW across sessions | R3 `sessionBoundary`, M3 timeout/configuration tests |
| G: callback A with current cache B, encoded UTC A | R3 `utcSnapshotAndWire` |
| H: validity flags, legal/illegal calendar, leap day/second, wire bounds | R3 `utcValidity` (23 calendar cases × 4 validity combinations) |
| J: week wrap/repeated iTOW and new-session reuse | R3 `sessionBoundary`, `repeatedStaleEpoch` |
| K: capture age survives delayed commit/radio, record remains after reboot | M4 `captureAgeSurvivesStorageAndRadioWait`; R2 `liveTxAdmissionAge` |
| L: first detect fails then succeeds; absent device exhausts bounded retries | R3 `detectionRetry`, including monotonic wrap and override precedence |
| Drain not successful; partial old DOP after transition | R3 `drainAndPartialDopBoundary` |

Session tests additionally inject a young, same-iTOW candidate with the prior
generation directly into the production manager to isolate the generation
predicate from clear/age protections. Normal lifecycle cases use public APIs.

The identical `test_delayed_pair.cpp` was also compiled against the original
`gnss_manager.h/.cpp` extracted read-only from **36ed21c**. It aborts at the
`!manager.takeFreshFixForTransmission(&fix)` assertion in `delayedPair(false,
5001)`, exit signal SIGABRT. This demonstrates that the regression detects the
original stale-PVT/late-DOP defect; it passes with R3.

Final validation commands, from repository root:

```sh
firmware/tests/run_host_tests.sh
(cd firmware && pio run)
git diff --check
git status --short
```

Host suites: M3, both R3 binaries, M4 storage/R1 regressions, production Nordic
backend, M5 and R2 radio/patch regressions. The runner compiles all C++ host
binaries with **ASan + UBSan**, **-Wall -Wextra -Werror**. Firmware build target:
`rak4630`, nRF52 platform 11.0.0, Arduino core 1.7.0, pinned GNSS 2.2.29.

- Firmware build: PASS. RAM **13,808 / 248,832 bytes (5.5%)**; Flash
  **135,496 / 815,104 bytes (16.6%)**.
- These are static linker figures, not measured runtime heap/stack peaks.
- Host suites, ASan, UBSan and -Wall -Wextra -Werror: **PASS**, runner exit 0.
- `git diff --check`: **PASS**. Physical validation remains pending.

## Changed files

- `firmware/include/gnss_config.h`
- `firmware/include/gnss_manager.h`
- `firmware/include/gnss_utc.h` (new)
- `firmware/include/position_flow.h`
- `firmware/include/radio_manager.h`
- `firmware/src/gnss_manager.cpp`
- `firmware/src/gnss_utc.cpp` (new)
- `firmware/src/position_flow.cpp`
- `firmware/src/radio_manager.cpp`
- `firmware/tests/m3/gnss_test_support.h` (shared harness extracted from M3)
- `firmware/tests/m3/stubs/SparkFun_u-blox_GNSS_Arduino_Library.h`
- `firmware/tests/m3/test_m3.cpp`
- `firmware/tests/m4/test_m4.cpp`
- `firmware/tests/m4/test_nrf_backend.cpp`
- `firmware/tests/r2/test_r2.cpp`
- `firmware/tests/r3/test_delayed_pair.cpp` (new)
- `firmware/tests/r3/test_r3.cpp` (new)
- `firmware/tests/run_host_tests.sh`
- `protocol/M2_POSITION_PACKET.md` (rules only, no layout/version change)
- `docs/milestones/M2.md` and `docs/milestones/M3.md` (R3 supersession links)
- `docs/audits/R3_GNSS_FRESHNESS_FIX.md` (this report)

## Physical validation and deliberate limits

Still required: real RAK12500 initial failure/retry/absence, actual I2C drain
and partial-message behavior, receiver restart/week boundary, outdoor TTFF and
UTC including leap-second behavior, repeated timeout/power cycles, short and
15-minute cadence, supply/current measurements, flash/radio contention expiry,
and peer LoRa reception of original coordinates/UTC/sequence/RSSI/SNR.

The local timestamp measures callback capture, not a hardware-latched antenna
measurement instant. The session guarantee relies on the inspected synchronous
ordered receiver stream and drain/boundary policy; it is not protection against
an arbitrary receiver replaying fabricated old epochs after the drain. There
is no GPS-week registry or trusted external UTC clock. Uint32 monotonic timing
retains M3's half-range/service-gap assumptions. Live age is checked at sender
admission; radio driver execution and over-the-air latency are not cancellable
freshness windows. New R3 production code adds no heap use; existing SparkFun
allocations remain.

Permanent I2C hard stalls, watchdog, shared WB_IO2 ownership, accelerometer,
geofence/LOST, BLE, Android/backend and relay redesign remain outside R3.
