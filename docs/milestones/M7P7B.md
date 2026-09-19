# M7P7B — First real BLE runtime + tracker admission policy

Status: **IMPLEMENTED ON BRANCH — host suite PASS; production RAK4630 build PASS; M7P7A guards re-verified intact; review-found advertising-payload bug (§8.1) fixed; independent-audit lifecycle findings (§8.3) fixed on host/build — physical rows 3–6 and 9 must be REPEATED on the new build (§9); PHYSICAL VALIDATION PARTIAL (§9: Debian PC/BlueZ only; several items PENDING). Full physical validation is NOT complete. Do not merge. Do not claim full physical BLE PASS.**

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

> **Superseded in part by §8.3.** §§2.2–2.4 and 3.1–3.2 record the original
> design. The independent audit showed that polling alone and framework-owned
> restart are insufficient; §8.3 is authoritative where they differ.

### 2.2 `Bluefruit.Periph.connected()` is the framework's own supported polled-read API

Every stock Adafruit Bluefruit example polls `Bluefruit.connected()`/
`Bluefruit.Periph.connected()` directly from `loop()`. Internally,
`BLEPeriph::connected()` sums small per-handle state written by Bluefruit's own
BLE event dispatch. That dispatch runs from a different FreeRTOS context than
the Arduino `loop()` task (the SoftDevice/SoC event pump; see §2.3), so this is
a genuine cross-task read — but it is the officially supported one: Bluefruit
exposes both a polled API and an optional callback API side by side for
exactly this use, and the polled form is what every reference sketch uses.
The polled read remains the path for a connection that stays active. It cannot
see a connection that starts and ends between two polls; §8.3 adds a minimal
disconnect-event handoff for that.

### 2.3 User callbacks run on a separate FreeRTOS task; `monotonic::nowMs()` must not

`cores/nRF5/utility/AdaCallback.c` shows `setConnectCallback`/
`setDisconnectCallback`-style user callbacks are queued and executed on a
dedicated `adafruit_callback_task`, not the Arduino loop task and not an ISR.
`monotonic_time.h`'s own doc comment is explicit: `nowMs()` is *"Loop task
only; not an ISR or cross-task clock."* Calling it from a Bluefruit callback
task would violate that contract. The original design avoided the question by not using callbacks. §8.3 does
register a disconnect callback, but it is only a counter increment inside
`taskENTER_CRITICAL()`; `BleAdmissionPolicy` and `monotonic::nowMs()` are still
driven exclusively from `loop()`/`setup()`.

### 2.4 `BLEAdvertising::restartOnDisconnect` — original reliance (SUPERSEDED, see §8.3)

`BLEAdvertising` defaults `_start_if_disconnect = true`: on
`BLE_GAP_EVT_DISCONNECTED`, if nothing else is connected, it calls
`start(_stop_timeout)` again automatically, from the SoC task, before the main
loop even ticks. M7P7B originally relied on this stock behavior (`start(0)`, no
library-owned timeout). The audit found that call ignores `start()`'s result
(finding 3), so production now sets `restartOnDisconnect(false)` and the loop
owns the restart (§8.3).

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

(Original model; the current state machine is in §8.3: `update()` now takes a
`BleAdmissionInput`, `kClose` is a repeated *request* until `confirmClosed()`,
and a `kStartAdvertising` action was added.)

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
  (§2.5), sets `restartOnDisconnect(false)` (§8.3; originally `true`, §2.4), starts
  advertising with no library-owned timeout, and calls
  `ble_admission.begin(now)`. `ble_ready` guards every later Bluefruit call
  the same way `radio_manager.begin()`'s result already guards radio use.
  No GATT service is added — a bare, named, connectable peripheral is
  sufficient to prove connect/disconnect lifecycle.
- `loop()`: if `ble_ready`, samples connection/advertising/disconnect-event
  state and feeds it into `ble_admission.update(input, now)`; acts on
  `kClose`/`kStartAdvertising` as described in §8.3. No TX guard needed — BLE (nRF52840 2.4GHz)
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

Cases 1–6 keep their original scenarios; after §8.3 their `kClose` assertions
also check the policy is *closing* (then `confirmClosed()` → closed) instead of
closed. Cases 7–15 and the extended startup scenarios are listed in §8.3.

`firmware/tests/startup/stubs/bluefruit.h` — minimal host stub of the Bluefruit
surface `main.cpp` calls. After the diagnostic change (§8.2) it is no longer
inert: `Advertising.start_result`/`running`/`start_calls` model
`BLEAdvertising::start()`/`isRunning()`, `Periph.connected_count` models
`BLEPeriph::connected()`, and `begin_result` models `Bluefruit.begin()`.
`firmware/tests/startup/test_startup.cpp` gained two scenarios, `advfail`
(advertising start fails) and `blefail` (`Bluefruit.begin()` fails), and now
asserts the BLE boot path, the `BLE?` output, connected-client behavior (window
never closes) and disconnect → fresh window → close. `run_host_tests.sh` runs
7 startup scenarios (`mutex gate queue lora success advfail blefail`) and its
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

### 8.2 BLE diagnostic + boot-path hardening (added for physical validation)

Adds a diagnostic and hardens one failure path. (Written before §8.3; the
"no callbacks" and framework-restart statements here are superseded by §8.3.)
The only behavior change is on failure: if the boot-time
`Bluefruit.Advertising.start(0)` returns false, admission is no longer opened
and BLE availability is no longer falsely claimed.

- New serial command `BLE?` prints one line:
  `BLE ready=<yes|no> advertising=<yes|no> connected=<n> policy=<open|closing|closed> initial_start=<ok|fail|not-attempted>`.
  `ready` = `Bluefruit.begin()` succeeded; `advertising` =
  `Bluefruit.Advertising.isRunning()`; `connected` = `Bluefruit.Periph.connected()`;
  `policy` = admission state (`open`, or after §8.3 also `closing`, or `closed`); `initial_start` = result of the one
  boot-time `Advertising.start(0)`.
- The boot-time `Advertising.start(0)` result is now checked. On failure the
  admission window is not opened, `BLE available` is not printed and
  `BLE advertising start failed` is printed instead (previously the result was
  ignored and availability claimed unconditionally).
- Host stub fidelity (Adafruit nRF52 1.7.0 `BLEAdvertising::_eventHandler`): a
  connection sets `_running=false`; a disconnect auto-restarts advertising when
  `restartOnDisconnect` is enabled. The startup test models this, so a
  connected peripheral reports
  `BLE ready=yes advertising=no connected=1 policy=open initial_start=ok`, the
  immediate post-disconnect state is
  `advertising=yes connected=0 policy=open`, and the fresh window's expiry ends
  at `advertising=no ... policy=closed`.
- Host: full `firmware/tests/run_host_tests.sh` **PASS** (exit 0), including all
  7 startup scenarios and the M7P7B policy test.
- `pio run -d firmware -e rak4630`: **PASS** — RAM 22,068 B / 248,832 B (8.9%),
  Flash 225,004 B / 815,104 B (27.6%).
- `pio run -d firmware -e rak4630_m7p7a_compile`: **PASS, unchanged** — RAM
  16,128 B, Flash 127,468 B.

### 8.3 Independent audit findings (pinned Adafruit nRF52 1.7.0) — fixed

An independent audit of `65893e2` found three related correctness issues. All
three were re-verified against the pinned source in
`framework-arduinoadafruitnrf52` (`BLEAdvertising.cpp`, `bluefruit.cpp`,
`cores/nRF5/utility/AdaCallback.c`) before coding.

**Root causes**

1. *Missed short connect/disconnect.* Lifecycle was derived only by polling
   `Periph.connected()`. A client that connected and disconnected between two
   `loop()` polls was never seen as connected, so the connected→disconnected
   edge never happened and no fresh ~10-minute window was granted, although
   Bluefruit itself saw the real disconnect and resumed advertising. This
   violated the core product contract.
2. *One-shot `stop()` failure.* The policy became closed when it emitted
   `kClose`, and `main.cpp` ignored `Advertising.stop()`'s result. Pinned
   `BLEAdvertising::stop()` returns before touching `_running` when
   `sd_ble_gap_adv_stop()` fails (e.g. a connection racing the stop), so the
   policy could read "closed" while advertising kept running, with no retry.
3. *Invisible auto-restart failure.* Pinned `BLEAdvertising::_eventHandler()`
   handles `BLE_GAP_EVT_DISCONNECTED` with
   `if (!_running && _start_if_disconnect) start(_stop_timeout);` and discards
   the result, so the policy could be open while nothing advertised. (The same
   ignored-result pattern exists in the framework's fast→slow
   `ADV_SET_TERMINATED` path, `_start(_slow_interval, 0)`; the loop's
   open-but-not-advertising reconciliation below covers it too.)

**Design choice verified in source:** `restartOnDisconnect(false)` sets
`_start_if_disconnect = false`, which makes the only automatic restart in
`_eventHandler()` a no-op. Production now sets it, so the loop is the single
owner of post-disconnect `Advertising.start(0)` and can see and retry failures.

**Implementation**

- `BleAdmissionPolicy` (`ble_admission_policy.{h,cpp}`): states `kClosed`
  (default, also "never begun") / `kOpen` / `kClosing`.
  `update(BleAdmissionInput{connected, disconnect_event, advertising_running}, now)`
  returns `kNone`, `kClose` or `kStartAdvertising`.
  - A `disconnect_event` (real disconnect since the last tick) grants a fresh
    window and cancels a pending close, whether or not polling ever saw the
    connection.
  - The polled connected→disconnected edge still grants a fresh window; while
    connected the timeout never closes.
  - Deadline → `kClosing` + `kClose`, repeated every `kRetryIntervalMs` (1 s)
    until `confirmClosed()`. `confirmClosed()` is ignored unless closing. Once
    closed it never reopens.
  - Open + not connected + advertising not running → `kStartAdvertising`,
    throttled to `kRetryIntervalMs`, only while the window is open; retries
    never extend the window (expiry → `kClose`, not another start).
  - All deadlines use `monotonic::reached()` (wrap-safe).
- `main.cpp`: `restartOnDisconnect(false)`; `Periph.setDisconnectCallback()`
  registered; the loop samples `{event counter, Advertising.isRunning(),
  Periph.connected()}` (advertising before connection, since the framework
  clears `_running` only after the connection object exists) and:
  - `kClose`: `Advertising.stop()`, then trusts *observed* state, not the
    return value — `confirmClosed()` and `BLE closed; ...` only if advertising
    is not running **and** nobody is connected; otherwise the request repeats.
  - `kStartAdvertising`: skipped if a client is connected; checks
    `Advertising.start(0)`; logs `BLE advertising restarted` on success and
    `BLE advertising restart failed; retrying` once per failure streak.
  - Boot-time `Advertising.start(0)` failure is unchanged: no `begin()`, so the
    policy stays closed (fail-closed), no window, no "BLE available".
- `BLE?` semantics unchanged (`advertising` = `isRunning()`, `connected` =
  `Periph.connected()`, `initial_start` = boot start only); `policy` gained the
  truthful intermediate value `closing`.

**Concurrency ownership**

- Pinned 1.7.0 runs the disconnect callback through `ada_callback()` on the
  dedicated FreeRTOS "Callback" task (not an ISR, not `loop()`).
- `onBleDisconnect()` only does `taskENTER_CRITICAL(); ++ble_disconnect_events;
  taskEXIT_CRITICAL();` — the same primitive `radio_manager.cpp` uses for its
  cross-task counters. It calls no `monotonic::nowMs()`, `BleAdmissionPolicy`,
  Serial, flash, radio or Bluefruit/SoftDevice API.
- `loop()` is the only consumer (snapshot under the same critical section) and
  the sole owner of policy, clock, Serial and every Bluefruit start/stop call.
  `ble_disconnect_events_seen` and the restart-log flag are loop-only. A
  counter (not a flag) is used so back-to-back disconnects are not collapsed
  before the loop observes them.
- A late event for a session already handled by polling only refreshes the
  fresh window by the callback latency; an event arriving while a *new* session
  is connected does not disconnect it.
- Residual, not solved here: `ada_callback_invoke()` drops the event on heap
  exhaustion (`rtos_malloc` failure). A fully-missed short session coinciding
  with that would not grant a fresh window.

**Tests added / changed**

- `test_m7p7b_ble_admission_policy.cpp`: original cases kept (adapted to
  closing → confirm); new cases 7–15: fully-missed short connection + real
  disconnect event grants a fresh window (with a no-event control); event+edge
  for the same disconnect and a stale event while reconnected; `kClose` is a
  repeated, throttled request until `confirmClosed()`; stays closed after
  confirmation; stray `confirmClosed()` is a no-op; a connection racing a
  pending close (polled and event-only) keeps the session admitted and grants a
  fresh window after the real disconnect; start retry throttled, bounded, never
  extends the window; never-begun policy stays closed; UINT32 wrap for windows,
  close/start retry, connected-across-rollover and event-across-rollover.
- `tests/startup/stubs/bluefruit.h`: models `stop()` failure (unchanged
  `_running`), failed `start()` leaving `_running`, a `stop()` racing a
  connection, `restartOnDisconnect(false)`, and `ada_callback`-style deferred
  disconnect-callback delivery. `tests/r2/stubs/FreeRTOS.h`: counts
  `taskENTER_CRITICAL()` calls.
- `test_startup.cpp` (`success`, `advfail`, `blefail` all still run): asserts
  `restartOnDisconnect` off and the callback registered; callback isolation (one
  balanced critical section, one counter increment, no log/clock/policy/
  Bluefruit effect); connect never closes; post-disconnect start success;
  post-disconnect start failure → truthful `advertising=no policy=open`, no busy
  retry, later retry success; a short connect+disconnect missed by polling still
  grants a fresh window; a connection racing close keeps the session (no false
  `BLE closed`, `policy=closing` then `open`); final expiry with `stop()` failing
  then succeeding (no false log before physical close; `advertising=no
  policy=closed` only after); closed stays closed; fail-closed boot
  (`advfail`/`blefail`) stays inert for many loop ticks.
- Mutation-checked: dropping the event handoff, confirming close
  unconditionally, and re-enabling framework restart each make the startup test
  fail.

**Build/host evidence (this fix)**

- `bash firmware/tests/run_host_tests.sh`: **PASS** (exit 0).
- `pio run -d firmware -e rak4630`: **PASS** — RAM 22,084 B / 248,832 B (8.9%),
  Flash 225,452 B / 815,104 B (27.7%) (was 22,068 B / 225,004 B: +16 B RAM,
  +448 B Flash).
- `pio run -d firmware -e rak4630_m7p7a_compile`: **PASS, unchanged** — RAM
  16,128 B, Flash 127,468 B.
- TLP v1 bytes/golden fixtures, RF, GNSS, storage formats/partitions, M7P7A
  flash/event ownership, role/capability separation, the one-client limit and
  the no-GATT/security/provisioning boundary are untouched (no diff in those
  sources; their host tests pass).

### Delta vs. `main@fb3a098` (M7P7A merged baseline: RAM 15,460 B / Flash 159,024 B)

Numbers after §8.2 (§8.3 above is current: RAM 22,084 B, Flash 225,452 B; §8.1's
22,060 B / 224,588 B are historical, taken at the review-fix point):

| | Baseline | M7P7B (current) | Delta |
| --- | --- | --- | --- |
| RAM | 15,460 B (6.2%) | 22,068 B (8.9%) | **+6,608 B** |
| Flash | 159,024 B (19.5%) | 225,004 B (27.6%) | **+65,980 B** |

This is the cost of linking the real Bluefruit52Lib/InternalFileSytem/GATT/GAP
stack for the first time in a shipped image (BLEDfu, BLEDis, BLEUart, BLEHid*,
BLEMidi, EddyStone and other Bluefruit52Lib service classes are part of the
library's own default composition and get linked regardless of whether this
slice's `main.cpp` uses them — no ORUN-side GATT service was added). Both
deltas remain comfortably within budget (8.9% of RAM, 27.6% of total flash /
28.5% of the M7P2 application policy ceiling of 790,528 B).

## 9. Physical-validation evidence

**Host/build PASS is not physical PASS.** Physical evidence below was collected
on one real RAK4631 with a **Debian PC (BlueZ) as the only BLE client/scanner**.
**Build note (§8.3):** rows 1–9 below were collected on the pre-audit firmware
(`65893e2`), where advertising restart after disconnect was framework-owned and
lifecycle was polled only. §8.3 changed that runtime path (loop-owned restart,
disconnect-event handoff, close confirmation). The table entries are left as
recorded, but rows 3, 4, 5, 6 and 9 do **not** cover the new build and must be
repeated (list after the summary). Rows 1 and 2 exercise unchanged code but
should be re-confirmed in the same session.

Legend: PASS = observed on hardware; PARTIAL = some evidence, not sufficient
for PASS; PENDING = not yet performed/inconclusive (not a failure); N/A = not
exercised by this slice.

**Full physical validation is NOT complete.**

| # | Item | Status | Evidence / reason |
| --- | --- | --- | --- |
| 1 | Real RAK4631 boots with BLE and advertises | **PASS** | Unit physically observed advertising as `ORUN-4B275BA5`. Same-boot `BLE?` returned `BLE ready=yes advertising=yes connected=0 policy=open initial_start=ok` while a BlueZ RF scan simultaneously saw `ORUN-4B275BA5` (firmware state and RF observation agree). |
| 2 | External scanner sees and can connect | **PASS (Debian PC/BlueZ only)** | A real PC/BlueZ BLE connection succeeded. |
| 2b | Phone scanner/connect (e.g. nRF Connect) | **PENDING** | Only the Debian PC/BlueZ was physically tested; no phone test done. |
| 3 | Connected state survives past original ~10-min no-client deadline | **PASS** | Connected client stayed connected beyond the original ~10-minute deadline. |
| 4 | Disconnect restores advertising / fresh admission window | **PASS** | After disconnect, advertising and a fresh admission window were restored. |
| 5 | BLE closes after a full window with no client | **PASS** | Later no-client fresh window closed normally: `BLE closed; no client connected within window`, then `BLE ready=yes advertising=no connected=0 policy=closed initial_start=ok`. |
| 6 | Reconnect during renewed window succeeds | **PENDING (not FAIL)** | BlueZ repeatedly produced transient `[NEW]`/`[DEL]` device behavior and `le-connection-abort-by-local` / `Device not available`, while firmware simultaneously reported `advertising=yes`, `connected=0`, `policy=open`. Root cause not determined (BlueZ device-cache/scan-state behavior is a candidate, unproven). **A second connection is NOT claimed PASS.** Retry with another client (phone) or after clearing the BlueZ device cache. |
| 7a | LoRa TX/RX coexistence with SoftDevice active | **PENDING** | Second node was not powered. |
| 7b | GNSS coexistence | **PENDING** | This physical unit reported `GNSS: not detected`. |
| 8 | Flash mutation (History/Config/Security) concurrency with BLE active | **PENDING** | No safe runtime mutation source available in this setup. |
| 9 | Reset/reboot recovery with BLE active in prior boot | **PARTIAL** | After firmware re-upload (DFU/reload), `BLE?` returned `BLE ready=yes advertising=yes connected=0 policy=open initial_start=ok`. This is **not** a clean power-cycle/reset PASS. |
| 10 | Relocated `InternalFS`/bond behavior | **N/A** | Bonding was not exercised. |
| 11 | Current/power measurement (advertising / connected / closed) | **PENDING** | Not measured. |

Physical checks to repeat on the §8.3 build: (a) disconnect → advertising
resumes via the *loop* restart (`BLE advertising restarted` in the log,
`BLE?` shows `advertising=yes policy=open`); (b) a very short connect/disconnect
still yields a fresh full window; (c) connected past the original deadline stays
open (row 3); (d) a no-client window still ends with `BLE closed; ...` and
`advertising=no policy=closed` (row 5); (e) reconnect in the renewed window
(row 6, still PENDING); (f) reset/reboot recovery (row 9). Stop/start failure
paths are host-modelled only and have no practical physical trigger.

Summary (as recorded on `65893e2`): items 1, 2 (PC/BlueZ), 3, 4, 5 are PASS on one unit with one client
type; 6, 2b, 7a, 7b, 8, 11 remain PENDING; 9 is PARTIAL; 10 is N/A. Do not
merge on the basis of this evidence alone; PENDING items must be reviewed with
real evidence first.

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
