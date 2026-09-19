# M7P7B — First real BLE runtime + tracker admission policy

Status: **IMPLEMENTED ON BRANCH — full host suite PASS; production RAK4630 build PASS with real production Bluefruit/InternalFS linkage (first time); M7P7A guards re-verified intact; a real review-found advertising-payload bug (§8.1) is fixed and re-verified; NO PHYSICAL VALIDATION YET. Do not merge. Do not claim physical BLE PASS.**

Baseline: `main@fb3a098c5bfc8b3488ba61a5ee28b57d8c5b0765`
(M7P7A merged plus post-merge architecture checkpoint).

Branch: `feat/m7p7b-ble-runtime-admission`

## 1. Purpose

M7P7A closed the flash/event arbitration prerequisites without enabling BLE.
This slice enables the first real, shipped `Bluefruit.begin()`/SoftDevice
runtime on RAK4630/RAK4631, implementing only the minimum tracker BLE
availability/admission behavior already specified in
`docs/architecture/ORUN_FIELD_NETWORK_DIAGNOSTICS_PLAN.md` §10 and `AGENTS.md`'s
BLE section:

- BLE becomes available after boot.
- No-client timeout is ~10 minutes; if nobody connects, BLE closes.
- A connected client suspends the timeout.
- Disconnect opens exactly one fresh ~10-minute window.
- Repeated disconnects never create permanent availability.
- One concurrent client only.

It does **not** implement: LoRa `OPEN_BLE`, Android/backend, provisioning/config
GATT, diagnostics GATT, DFU, secure envelope, multi-client BLE, or any
role/profile migration. It does not weaken the M7P7A single-physical-flash-
owner invariant.

## 2. Pre-coding audit (required by task; do not assume)

Read directly from the pinned, installed `framework-arduinoadafruitnrf52`
(`1.10700.0` / upstream 1.7.0) source, not from memory or the ADR alone.

### 2.1 SoftDevice cannot be safely disabled mid-boot and re-enabled

`grep -rn sd_softdevice_disable` across the entire framework finds exactly two
call sites:

- `cores/nRF5/wiring.c`'s `reset_mcu()` — disables SD, clears all NVIC
  interrupts, then unconditionally calls `NVIC_SystemReset()`. It is a
  point-of-no-return reset helper, not a resumable pause.
- `libraries/Bluefruit52Lib/src/services/BLEDfu.cpp` — disables SD immediately
  before jumping to the DFU bootloader (also a reset).

There is no code path in this framework that disables SoftDevice and expects
the application to keep running normally afterward. This directly confirms,
from source, what `ADR_M7_PERSISTENCE_LAYOUT.md` §9 already stated as a design
requirement: *"the backend mode (synchronous vs. asynchronous) is fixed for
the lifetime of one boot... live hot-swapping between modes within one boot is
explicitly not required or designed."* M7P7B does not attempt it.

**Conclusion: "BLE normally OFF" in this architecture cannot mean "SoftDevice
disabled."** It means "not advertising / not connectable," implemented as
`Bluefruit.Advertising.stop()`. SoftDevice itself, and its own baseline
RTC/scheduler housekeeping cost, remains resident for the rest of the boot
once `Bluefruit.begin()` succeeds. This is the audited, source-verified
interpretation the task asked for, not an assumption. Actual power-draw
magnitude in each state (advertising / connected / closed) is a **physical
measurement requirement**, listed in §8 below — host/build evidence cannot
establish it.

### 2.2 `Bluefruit.Periph.connected()` is the framework's own supported polled-read API

Every stock Adafruit Bluefruit example polls `Bluefruit.connected()`/
`Bluefruit.Periph.connected()` directly from `loop()`. Internally,
`BLEPeriph::connected()` sums small per-handle state written by Bluefruit's own
BLE event dispatch. That dispatch runs from a different FreeRTOS context than
the Arduino `loop()` task (the SoftDevice/SoC event pump; see §2.3), so this is
a genuine cross-task read — but it is the officially supported one: Bluefruit
exposes both a polled API and an optional callback API side by side for
exactly this use, and the polled form is what every reference sketch uses.
M7P7B relies on this designed usage rather than inventing new synchronization,
and does **not** register `setConnectCallback`/`setDisconnectCallback` at all
— see §3.

### 2.3 User callbacks run on a separate FreeRTOS task; `monotonic::nowMs()` must not

`cores/nRF5/utility/AdaCallback.c` shows `setConnectCallback`/
`setDisconnectCallback`-style user callbacks are queued and executed on a
dedicated `adafruit_callback_task`, not the Arduino loop task and not an ISR.
`monotonic_time.h`'s own doc comment is explicit: `nowMs()` is *"Loop task
only; not an ISR or cross-task clock."* Calling it from a Bluefruit callback
task would violate that contract. M7P7B's design (§3) avoids the question
entirely by not using connect/disconnect callbacks: the only new state
(`BleAdmissionPolicy`) is driven exclusively from the main loop, polling
`Bluefruit.Periph.connected()` each tick, with `monotonic::nowMs()` called only
from `loop()`/`setup()` as already required everywhere else in this codebase.

### 2.4 `BLEAdvertising::restartOnDisconnect` already does what "fresh window on disconnect" needs

`BLEAdvertising` defaults `_start_if_disconnect = true`: on
`BLE_GAP_EVT_DISCONNECTED`, if nothing else is connected, it calls
`start(_stop_timeout)` again automatically, from the SoC task, before the main
loop even ticks. M7P7B relies on this stock behavior (`start(0)`, no
library-owned timeout) instead of reimplementing it, and lets
`BleAdmissionPolicy::update()` open the actual bounded close-deadline on its
next tick once it observes the connected→disconnected edge.

### 2.5 Stock LED behavior is an avoidable power cost this slice would otherwise introduce

`bluefruit.cpp` blinks `LED_BLUE` on a FreeRTOS timer for the entire
advertising/connected duration by default (`_led_conn = true`,
`_startConnLed()`). Nothing in this milestone's spec needs a connection-status
LED, and `AGENTS.md`'s ANIMAL_TRACKER power policy is explicit about avoiding
unnecessary battery cost. `Bluefruit.autoConnLed(false)` is called before
advertising starts (§3) to remove this continuous GPIO toggle rather than
measure-and-accept it — this is not scope creep; it is directly caused by, and
scoped to, the exact runtime this slice enables.

## 3. Design

### 3.1 `BleAdmissionPolicy` — pure, host-tested admission state machine

New `firmware/include/ble_admission_policy.h` / `firmware/src/ble_admission_policy.cpp`.
Knows nothing about Bluefruit, SoftDevice, GATT, bonding, or authorization —
mirrors the existing `radio_listen_policy.h`/`geofence_runtime.h` pattern of a
small, pure, unit-testable policy class separate from hardware glue.

```text
begin(now)                         -> open, close_at = now + 10min
update(connected=false, now)       -> if now >= close_at: close (return kClose once)
update(connected=true,  now)       -> stays open indefinitely; never closes while connected
update(connected: true -> false)   -> edge: close_at = now + 10min (fresh window)
after close                        -> update() always returns kNone; does not reopen itself
```

`kNoClientTimeoutMs = 10 * 60 * 1000` (`ble_admission_config`).

### 3.2 `main.cpp` glue — the only place that touches `Bluefruit`

- `setup()`: `Bluefruit.begin()` is called **last**, strictly after
  `history.begin()`/`config_store.begin()`/`security_store.begin()` — see §2.1
  and the in-code comment: those synchronous backends fail closed if
  SoftDevice is already enabled when they run, and `Bluefruit.begin()` is what
  enables SoftDevice for the rest of the boot. On success: sets a compact
  identity-derived name (`ORUN-XXXXXXXX`, low 32 bits of the existing legacy
  device ID — already transmitted in the clear in every TLP v1 POSITION
  packet, so this adds no new exposure), disables the stock connection LED
  (§2.5), sets `restartOnDisconnect(true)` explicitly (§2.4), starts
  advertising with no library-owned timeout, and calls
  `ble_admission.begin(now)`. `ble_ready` guards every later Bluefruit call
  the same way `radio_manager.begin()`'s result already guards radio use.
  No GATT service is added — a bare, named, connectable peripheral is
  sufficient to prove connect/disconnect lifecycle.
- `loop()`: if `ble_ready`, reads `Bluefruit.Periph.connected() > 0` and feeds
  it into `ble_admission.update(connected, now)`; on `kClose`, calls
  `Bluefruit.Advertising.stop()`. No TX guard needed — BLE (nRF52840 2.4GHz)
  and LoRa (SX1262) are physically independent radios.
- `storage_flash_gate.pumpEvents()` is unchanged in `main.cpp` — the M7P7A
  bridge (Bluefruit declaring itself sole `sd_evt_get()` consumer, forwarding
  gate-owned completions) activates transparently the moment
  `Bluefruit.begin()` succeeds; this slice required no new wiring for it.

## 4. Compatibility / product impact

No intentional change to:

- TLP v1 bytes or golden fixtures;
- RF frequency/SF/BW, airtime or relay behavior;
- GNSS/tracking interval or acquisition logic;
- role compatibility, capability model;
- History/Config/Security on-flash formats or ownership;
- flash partition addresses;
- M7P7A's single-physical-flash-owner invariant and SoC-event-ownership
  hand-off (re-verified intact, §6).

Intentional new runtime behavior, scoped exactly to this slice: BLE
advertises after boot, is connectable for ~10 minutes at a time absent a
client, and SoftDevice is now genuinely enabled in production (not just
host/build-only as in M7P7A).

## 5. Security boundary (unchanged, verified against the diff)

- No BLE PIN/auth/application-token protocol invented.
- No GATT service exposing config/history/security data.
- No bond/phone-list persistence added to `SecurityStore` or anywhere else —
  `security_store.begin()` composition is untouched by this diff.
- No `K_root` export path.
- No LoRa `OPEN_BLE` command implemented — BLE availability is boot-driven
  only in this slice, matching the task's explicit exclusion.
- BLE bond/connection is not treated as, or conflated with, application
  authorization anywhere in this diff (there is no authorization surface yet
  for it to be conflated with — no GATT service exists to protect).

## 6. M7P7A invariant re-verification

- `rak4630_m7p7a_compile` target: **PASS, unchanged** RAM/Flash
  (16,128 B / 127,468 B) — the M7P7A framework patches, `check_exclusive_owner`
  and `check_application_ceiling` guards are exercised identically to before
  M7P7B and are unaffected by it (that target's `build_src_filter` does not
  include `main.cpp` or `ble_admission_policy.cpp`).
- Production build's `check_exclusive_owner` now actually evaluates its
  `Bluefruit linked` / `InternalFS linked` branches for the first time in a
  **shipped** build (previously dormant per M7P4/M7P7A's own docs, since
  nothing in production `main.cpp` included `<bluefruit.h>` before now) and
  passed — confirming the M7P7A patches are present and correct in the real
  production image, not just the isolated smoke target.
- `firmware/tests/m7/test_m7p7a_flash_gate.cpp` (8 scenarios, including the
  ownership-transfer-quarantine invariant): **PASS, unchanged**.

## 7. Tests added

`firmware/tests/m7/test_m7p7b_ble_admission_policy.cpp` — pure `BleAdmissionPolicy`
host coverage, no Bluefruit/Arduino dependency at all (only `monotonic::reached()`,
a `constexpr` inline function):

1. Opens at `begin()`; no premature close before the window elapses.
2. No client connects before the ~10-minute deadline: closes exactly once,
   stays closed afterward (does not reopen itself, including if a connection
   is later observed).
3. A connected client suspends the timeout even long past the original
   deadline.
4. After disconnect, a fresh ~10-minute window starts, measured from the
   disconnect tick, not from boot.
5. Five repeated connect/disconnect cycles: each disconnect grants exactly one
   bounded fresh window; the final one still closes on schedule if nobody
   reconnects — repeated cycles never accumulate into permanent availability.
6. Reconnect during the renewed (post-disconnect) window works, and correctly
   suspends that window's own deadline in turn.

`firmware/tests/startup/stubs/bluefruit.h` — new minimal host stub (`Bluefruit`
global with `begin()`/`setName()`/`autoConnLed()`/`Advertising.{start,stop,
restartOnDisconnect}()`/`Periph.connected()`), needed because
`firmware/tests/startup/test_startup.cpp` compiles the real `main.cpp` directly
against host stub headers. The stub is inert (fixed return values); it asserts
nothing about BLE state, since `test_startup.cpp`'s own scope is radio/gate/
GNSS/PositionFlow/journal boot behavior, unrelated to BLE. `run_host_tests.sh`'s
startup-test source list gained `firmware/src/ble_admission_policy.cpp`.

## 8. Validation evidence

### 8.1 Independent review finding — fixed

A `/code-review medium` pass (after implementation + host/build PASS, per this
milestone's own review discipline) found one real bug, verified directly
against the pinned, installed Bluefruit52Lib source before fixing:
`setup()` called `Bluefruit.setName(name)` but never called
`Bluefruit.Advertising.addFlags(...)`/`Bluefruit.Advertising.addName()`
before `Advertising.start(0)`.

`AdafruitBluefruit::setName()` only writes the GAP Device Name
*characteristic* via `sd_ble_gap_device_name_set()` — readable only **after**
a client connects. It does not touch the advertising PDU at all.
`BLEAdvertisingData::addName()` is the only call that actually copies the
name into the broadcast payload, and `begin()` does not call it implicitly.
Every stock Adafruit peripheral example (`adv_advanced.ino`,
`blehid_camerashutter.ino`, etc.) calls both `addFlags()` and `addName()`
explicitly before `Advertising.start()`; the diff omitted both, so the
advertising packet would have shipped with **zero AD structures** — a phone
scanner would have seen an anonymous device (bare MAC, no name, not marked
general-discoverable), directly failing this milestone's own physical
validation item 2 (§9) and making the logged `BLE available name=...` line
not actually scan-visible. This could not be caught by the host test suite,
since the host `Bluefruit` stub is inert and does not model advertising
payload contents.

**Fix:** added `Bluefruit.Advertising.addFlags(BLE_GAP_ADV_FLAGS_LE_ONLY_GENERAL_DISC_MODE)`
and `Bluefruit.Advertising.addName()` in `setup()`, between `setName()` and
`Advertising.start(0)`, matching stock example usage exactly. Added
`addFlags()`/`addName()` to the host stub
(`firmware/tests/startup/stubs/bluefruit.h`) so `test_startup.cpp` keeps
compiling. Re-verified: `arm-none-eabi-nm` on the rebuilt production `.elf`
now shows `BLEAdvertisingData::addFlags`/`::addName` linked (`T`, defined);
full host suite re-run **54/54 PASS**; production build re-run **PASS**
(RAM unchanged at 22,060 B, Flash +192 B to 224,588 B — the two new calls'
own code size). No other findings from this review.

Canonical Debian checkout, this exact (post-fix) diff:

- `g++` direct compile/run of `test_m7p7b_ble_admission_policy.cpp`: **PASS**
  (ASan/UBSan, `-Wall -Wextra -Werror`, zero warnings).
- `bash firmware/tests/run_host_tests.sh`: **PASS, 54/54**, including the new
  M7P7B policy test, the updated startup scenarios (all 5), and every
  pre-existing M0–M7P7A regression, unchanged.
- Compiler warnings review: grepped the full production build log for
  `warning`; the only hits are the two pre-existing `SX126x-Arduino` vendor
  warnings already noted in `docs/milestones/M7P7A.md` (`-Wsign-compare` in
  `SimpleTimer.cpp`, `#warning USING RAK4630` in `RAK4630_MOD.cpp`) — zero
  warnings from `main.cpp` or `ble_admission_policy.cpp`.
- `pio run -d firmware -e rak4630`: **PASS**.
  - RAM: **22,060 B / 248,832 B (8.9%)**
  - Flash: **224,588 B / 815,104 B (27.6%)**
  - `check_exclusive_owner`: **PASS** — for the first time in a shipped build,
    its `Bluefruit linked` / `InternalFS linked` branches actually ran (not
    dormant) and confirmed the M7P7A patches are present.
  - `check_application_ceiling`: **PASS** (224,588 B is well under the M7P2
    policy ceiling of 790,528 B for the `0x026000..0x0E7000` application
    region).
  - `arm-none-eabi-nm` on the linked production `.elf` confirms `Bluefruit`
    (`B`, defined), `InternalFS` (`B`, defined), `AdafruitBluefruit::begin`
    and `::autoConnLed` (`T`, defined), `BLEAdvertisingData::addFlags`/
    `::addName` (`T`, defined, §8.1), all five M7P7A `orun_flash_*` strong
    hooks (`T`, defined), and `flash_nrf5x_erase`/`_write`/`_event_cb`
    (`T`, defined) — this is genuine production linkage, not merely
    compile-smoke evidence.
- `rak4630_m7p7a_compile`: **PASS, unchanged** (§6).

### Delta vs. `main@fb3a098` (M7P7A merged baseline: RAM 15,460 B / Flash 159,024 B)

| | Baseline | M7P7B | Delta |
| --- | --- | --- | --- |
| RAM | 15,460 B (6.2%) | 22,060 B (8.9%) | **+6,600 B** |
| Flash | 159,024 B (19.5%) | 224,588 B (27.6%) | **+65,564 B** |

This is the cost of linking the real Bluefruit52Lib/InternalFileSytem/GATT/GAP
stack for the first time in a shipped image (BLEDfu, BLEDis, BLEUart, BLEHid*,
BLEMidi, EddyStone and other Bluefruit52Lib service classes are part of the
library's own default composition and get linked regardless of whether this
slice's `main.cpp` uses them — no ORUN-side GATT service was added). Both
deltas remain comfortably within budget (8.9% of RAM, 27.5% of total flash /
28.4% of the M7P2 application policy ceiling).

## 9. Physical-validation boundary

**Host/build PASS is not physical PASS.** No physical behavior is claimed by
this branch. The following require a real RAK4631 and are explicitly not
performed here, in the order the task requested — one step at a time for the
owner/operator, not invented or assumed:

1. Real RAK4631 boots with BLE; USB Serial confirms `BLE available name=...`.
2. A phone's BLE scanner (e.g. nRF Connect) sees and can connect to the
   advertised name.
3. Connected state survives past the original ~10-minute no-client deadline
   (i.e. BLE does not disconnect/close merely because the original window
   would have expired).
4. Disconnect starts a fresh ~10-minute window (BLE remains connectable
   immediately after disconnect, not closed).
5. BLE closes (no longer visible/connectable) after a full ~10-minute window
   with no client.
6. Reconnect during a renewed (post-disconnect) window succeeds.
7. LoRa TX/RX, GNSS acquisition and tracking continue correctly with
   SoftDevice active (no regression from the M0–M7P7A physically-validated
   behavior).
8. History/Config/Security flash activity does not deadlock or corrupt with
   BLE active — ideally exercised concurrently with an active BLE connection
   and ongoing store-before-send traffic.
9. Reset/reboot recovery with BLE having been active in the prior boot.
10. Relocated `InternalFS`/bond behavior if bonding is actually exercised
    (this slice does not add pairing/bonding UX, but stock BLE bonding may
    still be reachable via a generic BLE client depending on Security
    settings — worth an explicit physical check).
11. Current/power measurement: advertising-only, connected, and closed
    states, to determine whether the audited (§2.1) "no SoftDevice disable"
    behavior is an acceptable power posture for ANIMAL_TRACKER, or whether a
    later milestone must revisit it.

None of these are claimed PASS by this document. Do not merge until they are
reviewed with real evidence.

## 10. Explicit non-claims / deferred work

Recorded, not hidden:

- LoRa `OPEN_BLE` command — requires the secure downlink/authenticated command
  path, which does not exist yet. Deferred, not implemented, not stubbed with
  an unauthenticated shortcut.
- Any BLE GATT service (provisioning, config, diagnostics, DFU).
- Multi-client BLE.
- Role-differentiated BLE availability (BASE/MOBILE "continuous" per
  `AGENTS.md`) — this slice applies the same tracker admission policy to every
  role uniformly; role-aware BLE policy is future work.
- A stalled-session watchdog for a connected client making no progress
  (`ORUN_FIELD_NETWORK_DIAGNOSTICS_PLAN.md` §10 already flags this as a
  distinct future requirement, not part of this slice).
- Bonding/pairing UX, first-phone ownership, or any authorization model tied
  to BLE connection/bonding.
