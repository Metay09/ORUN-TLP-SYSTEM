# M7P7B — First real BLE runtime + tracker admission policy

Status: **MERGED TO `main` via PR #22 at `3b7eb0e6ae0275e6bf3e95f9c19f55c108cec87a`. Host/build validation PASS at the last code-bearing head; physical evidence closes the current direct-event BLE lifecycle, scoped stock-bond persistence, BLE/LoRa coexistence and the shared ConfigStore→FlashMutationGate→SoftDevice async mutation path. Quantitative power remains DEFERRED (not PASS) because no measurement equipment is available; GNSS coexistence remains BLOCKED on the tested unit and was owner-waived only as an M7P7B merge blocker. Between-poll short-session timing and advertising start/stop fault injection remain host-only. Exact evidence boundaries and non-claims are preserved below.**

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
see a connection that starts and ends between two polls; §8.3/§8.5 add a minimal
disconnect-event handoff for that.

### 2.3 Bluefruit callbacks run off the loop task; `monotonic::nowMs()` must not

`cores/nRF5/utility/AdaCallback.c` shows `setConnectCallback`/
`setDisconnectCallback`-style Periph callbacks are queued (heap-allocated,
droppable) and executed on a dedicated `adafruit_callback_task`; the global
`Bluefruit.setEventCallback()` callback instead runs directly on Bluefruit's BLE
event task (§8.5). Neither is the Arduino loop task, and neither is an ISR.
`monotonic_time.h`'s own doc comment is explicit: `nowMs()` is *"Loop task
only; not an ISR or cross-task clock."* Calling it from a Bluefruit callback
context would violate that contract. The original design avoided the question by
not using callbacks. §8.5 registers the direct global event callback, but it is
only a counter increment inside `taskENTER_CRITICAL()`; `BleAdmissionPolicy` and
`monotonic::nowMs()` are still driven exclusively from `loop()`/`setup()`.

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
  device ID, which is already sent in the clear in every TLP v1 POSITION
  packet). No new secret or credential is disclosed, but BLE advertising is a
  **new** surface: the identifier is now discoverable and correlatable by any
  nearby commodity BLE scanner (as `ORUN-4B275BA5` was by a phone). This is not
  a privacy redesign; it is recorded so the exposure is not understated),
  disables the stock connection LED
  (§2.5), sets `restartOnDisconnect(false)` (§8.3; originally `true`, §2.4), starts
  advertising with no library-owned timeout, and calls
  `ble_admission.begin(now)`. `ble_ready` guards every later Bluefruit call
  the same way `radio_manager.begin()`'s result already guards radio use.
  No ORUN-specific application GATT service is added — a bare, named,
  connectable peripheral is sufficient to prove connect/disconnect lifecycle.
  (The standard Generic Access `0x1800` and Generic Attribute `0x1801`
  services are still present; see §5.)
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

## 5. Security boundary (verified against the diff and the pinned framework)

What M7P7B adds, and does not add:

- **No ORUN-specific application GATT service** is added. No config, history,
  security, provisioning or diagnostics data is exposed over GATT.
- The standard **Generic Access (`0x1800`) and Generic Attribute (`0x1801`)**
  services are present (SoftDevice/Bluefruit stock). They were physically
  observed in nRF Connect on `9673f49` (§9, row 2b) and are the only services
  that were visible.
- No ORUN pairing/ownership/authorization design: no PIN, no application
  authentication, no first-phone ownership, no provisioning, no secure GATT
  data, no application-token protocol. M7P7B does **not** define ORUN
  ownership or authorization.
- No bond/phone-list persistence was added to `SecurityStore` or anywhere else
  — `security_store.begin()` composition is untouched by this diff.
- No `K_root` export path.
- No LoRa `OPEN_BLE` command — BLE availability is boot-driven only in this
  slice.

What still exists at framework level (pinned Adafruit nRF52 1.7.0, verified in
`bluefruit.cpp`/`BLESecurity.cpp` and in the linked ELF, §8.4):

- `Bluefruit.begin()` calls `Security.begin()`, so the stock
  Bluefruit/SoftDevice pairing/bonding machinery is initialised and linked
  (`BLESecurity::_eventHandler`, `BLEConnection::bonded`/`saveBondKey`/
  `loadBondKey`/`removeBondKey`, `BLEConnection::secured`). Stock defaults are
  Just Works (`bond=1`, `mitm=0`, `io_caps=NONE`, LESC supported), i.e.
  unauthenticated pairing; M7P7B does not change them.
- The path is reachable in this runtime: `Bluefruit.begin()` →
  `Security.begin()`, and `Bluefruit.begin()` → `bond_init()` →
  `InternalFS.begin()` (`bluefruit.cpp`, `utility/bonding.cpp`). A peer *can*
  initiate stock pairing/bonding, and a successful bond can write keys/CCCD
  state to the `InternalFS` relocated by the M7P4 patch, through the M7P7A
  shared physical-flash ownership. M7P7B neither prevents nor uses it. That
  path physically exercises the relocated bond partition, M7P7A flash ownership
  and Bluefruit/`InternalFS` SoftDevice-event interaction, and it is **not yet
  exercised on hardware** (§9, row 10: PENDING).
- No ORUN pairing/provisioning UX is implemented.
- **A bare BLE connection, and a framework bond, are not ORUN application
  authorization** and must not be described or relied on as such. Any future
  ORUN authorization/ownership model is a separate milestone.
- Physical validation did not create a bond: nRF Connect showed
  `CONNECTED` / `NOT BONDED` throughout. This is "not yet tested", not "not
  applicable" (§9, row 10; §11).

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

Canonical Debian checkout, this exact (post-fix) diff — **historical figures at the §8.1 review-fix point, superseded by §8.3/§8.4; not current**:

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

### 8.2 BLE diagnostic + boot-path hardening (added for physical validation) — historical

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
- `pio run -d firmware -e rak4630`: **PASS** — *(historical, at the §8.2 point; superseded by §8.3)* RAM 22,068 B / 248,832 B (8.9%),
  Flash 225,004 B / 815,104 B (27.6%).
- `pio run -d firmware -e rak4630_m7p7a_compile`: **PASS, unchanged** — RAM
  16,128 B, Flash 127,468 B.

### 8.3 Independent audit findings (pinned Adafruit nRF52 1.7.0) — fixed

> **Callback path superseded by §8.5.** §8.3 originally registered
> `Periph.setDisconnectCallback()` (an `ada_callback` path). Production now
> uses `Bluefruit.setEventCallback()`; where the text below says otherwise, §8.5
> is authoritative.

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
- `main.cpp`: `restartOnDisconnect(false)`; a disconnect handoff callback
  registered (§8.3: `Periph.setDisconnectCallback()`; **now**
  `Bluefruit.setEventCallback(onBleEvent)`, §8.5); the loop samples `{event counter, Advertising.isRunning(),
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

- Pinned 1.7.0 runs the *Periph* disconnect callback through `ada_callback()`
  on the dedicated FreeRTOS "Callback" task; that path was replaced (§8.5). The
  current `onBleEvent()` runs on the BLE event task (not an ISR, not `loop()`).
- `onBleEvent()` on `BLE_GAP_EVT_DISCONNECTED` only does
  `taskENTER_CRITICAL(); ++ble_disconnect_events; taskEXIT_CRITICAL();` — the same primitive `radio_manager.cpp` uses for its
  cross-task counters. It calls no `monotonic::nowMs()`, `BleAdmissionPolicy`,
  Serial, flash, radio or Bluefruit/SoftDevice API.
- `loop()` is the only consumer (snapshot under the same critical section) and
  the sole owner of policy, clock, Serial and every Bluefruit start/stop call.
  `ble_disconnect_events_seen` and the restart-log flag are loop-only. The
  handoff is a bounded integer counter, but `loop()` reduces
  `counter != seen` to **one** logical `disconnect_event` per tick: several
  disconnects observed since the previous tick are *not* individually
  preserved. That is sufficient for the admission semantics, which only need one
  fresh window measured from the latest observation.
- A late event for a session already handled by polling only refreshes the
  fresh window by the callback latency; an event arriving while a *new* session
  is connected does not disconnect it.
- The residual recorded here originally (`ada_callback_invoke()` dropping the
  event on heap exhaustion) no longer applies to the disconnect path: §8.5.

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
  connection, `restartOnDisconnect(false)`, and (§8.3) `ada_callback`-style
  deferred Periph disconnect-callback delivery — extended in §8.5 with the direct
  global event callback. `tests/r2/stubs/FreeRTOS.h`: counts
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
  the no-ORUN-application-GATT/authorization/provisioning boundary (§5) are untouched (no diff in those
  sources; their host tests pass).

### 8.4 Resource figures and linked-ELF evidence

Production build at the **§8.5 (current)** head — see §8.5 for the current
figures. Figures below are for audit-fix head `9673f49`, the image that was
physically uploaded and exercised in §9.1 (**historical for the current build**):

- RAM: **22,084 B / 248,832 B (8.9%)**
- Flash: **225,452 B / 815,104 B (27.7%)**
- `arm-none-eabi-size firmware.elf`: text 223,296 / data 2,156 / bss 233,364.

Delta vs. `main@fb3a098` (M7P7A merged baseline: RAM 15,460 B / Flash 159,024 B):

| | Baseline | M7P7B (`9673f49`, historical) | Delta |
| --- | --- | --- | --- |
| RAM | 15,460 B (6.2%) | 22,084 B (8.9%) | **+6,624 B** |
| Flash | 159,024 B (19.5%) | 225,452 B (27.7%) | **+66,428 B** |

`rak4630_m7p7a_compile` is unchanged: RAM 16,128 B / Flash 127,468 B.

Historical intermediate values, superseded and not current: 22,060 B / 224,588 B
(§8.1 review-fix point) and 22,068 B / 225,004 B (§8.2 diagnostic point,
+6,608 B / +65,980 B vs. baseline).

**What the linked production ELF actually contains** (`arm-none-eabi-nm -C
firmware.elf`, defined symbols; nothing below is inferred from library
defaults):

- **Optional Bluefruit service classes are NOT linked.** Zero symbols of any
  kind match `BLEDfu`, `BLEUart`, `BLEHid*`, `BLEMidi`, `EddyStone`, `BLEBas`,
  `BLEHrm`, `BLECts`, `BLEAncs`, `BLEBeacon`, or `BLEDis` (device-information
  service; the only `BLEDis*` substring hits are `BLEDiscovery`), nor any
  `BLEClient*` optional client-service class (`BLEClientUart/Dis/Bas/Cts/
  HidAdafruit`), nor any non-client `BLEService` symbol. An earlier revision of
  this document claimed these were linked merely because Bluefruit is enabled;
  that was incorrect and is withdrawn. The flash delta must therefore **not** be
  attributed to optional services.
- **Linked and verified present:** the `Bluefruit` (`AdafruitBluefruit`) and
  `InternalFS` objects (`B`); `AdafruitBluefruit::begin`/`setName`/
  `autoConnLed`; GAP/GATT core: `BLEPeriph` (incl. `begin`, `connected`; at `9673f49` also
  `setDisconnectCallback`, no longer linked after §8.5), `BLEAdvertising` (`start`/`stop`/`isRunning`/
  `restartOnDisconnect`/`_eventHandler`), `BLEAdvertisingData::addFlags`/
  `addName`, `BLEGatt`, `BLEConnection`, `BLEUuid`, `BLECharacteristic`,
  `BLEClientService`/`BLEClientCharacteristic`; central-side core classes
  `BLECentral`, `BLEScanner`, `BLEDiscovery` (linked; `Bluefruit.begin()` in
  `main.cpp` uses the default `central_count = 0`, so the Central role is not
  started); `BLESecurity` (`begin`, `_eventHandler`, `_encrypt`,
  `resolveAddress`) and the bonding helpers `BLEConnection::bonded`/
  `saveBondKey`/`loadBondKey`/`removeBondKey`/`saveCccd`/`secured`;
  `Adafruit_LittleFS`/`InternalFileSystem`/`Adafruit_LittleFS_Namespace::File`;
  the M7P7A `orun_flash_*` hooks and `flash_nrf5x_*` (§8.1).
- This is a *presence* list only. The flash/RAM delta is the measured
  difference above; it is **not decomposed** per component here, and no
  per-component size is claimed.

### 8.5 Direct Bluefruit event handoff (blocking audit fix) — current head

**Finding.** After §8.3 the disconnect notification still travelled through
`Periph.setDisconnectCallback()`, i.e. Bluefruit's `ada_callback()` queue.
`AdaCallback.c` allocates each callback with `rtos_malloc()`, so
`ada_callback_invoke()` can fail and drop the event, and delivery can also be
delayed on the "Callback" task. A short connect+disconnect that fits entirely
between two loop polls depends solely on that event; if it is dropped or
arrives after the old deadline was already confirmed closed, the required fresh
post-disconnect ~10-minute window is lost.

**Change (production).**

- Old: `Bluefruit.Periph.setDisconnectCallback(onBleDisconnect)` (through
  `ada_callback`, heap-allocated, droppable, runs on the "Callback" task).
- New: `Bluefruit.setEventCallback(onBleEvent)`. `onBleEvent(ble_evt_t*)` reads
  only `evt->header.evt_id`; on `BLE_GAP_EVT_DISCONNECTED` it does
  `taskENTER_CRITICAL(); ++ble_disconnect_events; taskEXIT_CRITICAL();`, and
  returns immediately for every other event. It calls no `monotonic::nowMs()`,
  `BleAdmissionPolicy`, Serial, flash, radio or Bluefruit API. `loop()` remains
  the sole owner of time, policy, advertising start/stop, logging, flash, radio
  and application state. `restartOnDisconnect(false)` is unchanged.
- Related, same seam: in the loop's `kClose` branch the disconnect counter is
  read *after* the connection state, and the close is left unconfirmed if a
  disconnect event is pending. This covers a whole connect+disconnect completing
  between `Advertising.stop()` and the loop's state reads (neither read would
  show the client although a real disconnect is owed a fresh window). The next
  tick consumes the event, cancels the close and restarts advertising.
- The counter wording was corrected (§8.3): it is a bounded integer counter that
  `loop()` reduces to one logical event per tick; multiple disconnects are not
  individually preserved.

**Order-of-events evidence (pinned Adafruit nRF52 1.7.0,
`libraries/Bluefruit52Lib/src/bluefruit.cpp`, `AdafruitBluefruit::_ble_handler`).**
For a disconnect, in this order, all on the BLE event task
(`adafruit_ble_task` → `sd_ble_evt_get` → `_ble_handler`):

1. `conn->_eventHandler(evt)` (line 789) — `BLEConnection.cpp` marks
   `_connected = false` on `BLE_GAP_EVT_DISCONNECTED`; `Security._eventHandler`.
2. The `BLE_GAP_EVT_DISCONNECTED` case (lines 837–856): connection LED, the
   `Periph._disconnect_cb` `ada_callback(...)` dispatch (unused by ORUN now),
   then `delete _connection[conn_hdl]; _connection[conn_hdl] = NULL`.
3. `Advertising._eventHandler(evt)` (line 882) — the framework's own restart is a
   no-op because production sets `restartOnDisconnect(false)`.
4. `_conn_hdl` reset, `Periph._eventHandler`, `Central._eventHandler`,
   `Discovery`/`Gatt` handlers.
5. **`if (_event_cb) _event_cb(evt);` (line 934) — last.**

So the global callback runs after Bluefruit's own disconnect state handling for
that event, directly (no `ada_callback`, no heap allocation), and
`Periph.connected()` no longer counts the connection when it fires. It is
invoked for every BLE event, hence the strict `evt_id` filter. Central role is
not started (`central_count = 0`), so a `BLE_GAP_EVT_DISCONNECTED` here is a
peripheral-role disconnect. `Bluefruit.setEventCallback` has no other user in
the framework or ORUN sources.

**Tests.** `tests/startup/stubs/bluefruit.h` now distinguishes
`Periph.setDisconnectCallback()` (queued, droppable, deferred) from
`Bluefruit.setEventCallback()` (direct, synchronous, invoked last for
connect/disconnect, with an `event_delivery` knob). `test_startup.cpp`:

- asserts production registers `onBleEvent` and leaves
  `Periph.disconnect_cb == nullptr`, with no Periph callback ever queued;
- a short connect+disconnect entirely between two loop polls, at the edge of the
  old deadline, still grants a full fresh window through the direct callback
  (BLE stays open past the *original* deadline, closes only after the fresh one);
- callback isolation: `onBleEvent` on disconnect changes only the counter (one
  balanced critical section; no log, clock, policy, advertising or flash/radio
  effect); connect/other event ids change nothing and take no critical section;
- new scenario `noevent` (negative control, added to `run_host_tests.sh`): the
  same short cycle with delivery disabled loses the window and BLE closes at the
  original deadline — the opposite of the main flow;
- a connect+disconnect completing between `stop()` and the loop's reads at the
  deadline leaves the close unconfirmed, then restarts advertising with a fresh
  window;
- all previous lifecycle coverage is unchanged (boot window, connected
  suspends, disconnect fresh window, no-client close, stop-failure and
  start-failure retry, close/connect race, UINT32 wrap in the policy test,
  initial advertising failure, `Bluefruit.begin()` failure).

Mutation-checked (host suite fails in each case): removing the
`setEventCallback` registration; disabling event delivery in the stub; using the
Periph disconnect callback instead; dropping the pending-event guard on close
confirmation.

**Build/host evidence.**

- `bash firmware/tests/run_host_tests.sh`: **PASS** (exit 0; 8 startup scenarios
  `mutex gate queue lora success advfail blefail noevent`).
- `pio run -d firmware -e rak4630`: **PASS** — RAM **22,084 B / 248,832 B
  (8.9%)**, Flash **225,484 B / 815,104 B (27.7%)**; `check_exclusive_owner` and
  `check_application_ceiling` PASS.
- Delta vs. `main@fb3a098` (RAM 15,460 B / Flash 159,024 B): RAM **+6,624 B**,
  Flash **+66,460 B**. Relative to `9673f49` (225,452 B): +0 B RAM, +32 B Flash.
- `pio run -d firmware -e rak4630_m7p7a_compile`: **PASS, unchanged** — RAM
  16,128 B, Flash 127,468 B.
- ELF (`arm-none-eabi-nm -C`): `AdafruitBluefruit::setEventCallback` and
  `onBleEvent(ble_evt_t*)` are linked; `BLEPeriph::setDisconnectCallback` is no
  longer linked. The optional-service result of §8.4 is unchanged (zero
  `BLEDfu`/`BLEUart`/`BLEHid*`/`BLEMidi`/`EddyStone`/`BLEBas`/`BLEHrm`/`BLECts`/
  `BLEAncs`/`BLEBeacon`/`BLEDis` symbols). `ada_callback_*` remains linked as
  framework code but is not on the ORUN disconnect path.
- No change to TLP v1 bytes/golden fixtures, RF, GNSS, storage layout/partitions,
  M7P7A flash/event ownership, timeout constants, single-client configuration
  or role/capability semantics.

**Physical invalidation.** This change alters only how the disconnect event is
acquired. Evidence from `9673f49` for boot advertising, phone scan, initial
connection, connected past the deadline, no-client close and clean cold boot
remains useful; the post-disconnect lifecycle (§9.0) must be re-run on this
build.

### 8.6 Temporary flash-concurrency probe (test-only; physical run PASS)

Row 8 of §9 (real History/Config/Security flash mutation while BLE is active)
needs a real mutation source; production has none. A **temporary, test-only**
probe was added for it. It is not a production feature.

- **Containment.** Only PlatformIO env `rak4630_m7p7b_flash_probe` (extends
  `rak4630`, adds `-D ORUN_M7P7B_FLASH_PROBE=1`) compiles the `FLASH PROBE`
  serial command and its output. The production `rak4630` ELF contains zero
  `FLASH PROBE` strings and zero probe symbols; the probe ELF contains both.
  Production RAM/Flash are unchanged by this work (22,084 B / 225,484 B; text
  223,328 B, identical to before).
- **Path exercised.** Real `ConfigStore` (`requestSave`/`poll`/`takeSaveResult`)
  → `FlashMutationGate` config port → `sd_flash_page_erase`/`sd_flash_write` and
  the Bluefruit-forwarded completion events (M7P7A bridge), driven by the normal
  loop (`pumpEvents()` then `config_store.poll()`). The probe calls no `sd_flash_*`,
  touches no History/Security/InternalFS/bond partition, adds no flash owner,
  mutex or event consumer, uses no `delay()`/blocking/heap, and `onBleEvent()` is
  unchanged. ConfigStore's own write path already reads back and compares each
  saved record before `finishSave()`; the probe relies on that.
- **Field used.** `battery_capacity_mah` (bit 0 toggled). Verified: it is
  consumed by nothing but ConfigStore/ConfigFormat (no runtime power, GNSS, radio
  or tracking behavior reads it; `main.cpp` reads only
  `tracking_interval_seconds`). It always differs from the original and is fully
  reversible. On-flash format, generation ping-pong and partition are unchanged
  (the run advances the generation by 2 and leaves the original values in effect).
- **Command.** `FLASH PROBE` (11 bytes; fits the existing 24-byte command
  buffer, which already rejects overlong input). Refused with `FLASH PROBE
  REJECT reason=...` and no config change unless: probe idle, ConfigStore ready
  and not busy, BLE runtime ready, and exactly one peripheral connection. A stale
  unread ConfigStore result (nothing in this firmware consumes them) is
  dropped at start so it cannot be mistaken for the probe's.
- **State machine** (`firmware/include/m7p7b_flash_probe.h`, stepped once per
  loop tick after `config_store.poll()`; host-tested in
  `tests/m7/test_m7p7b_flash_probe.cpp`):
  `IDLE` → snapshot original config, `ble_disconnect_events`, gate config
  diagnostics; `requestSave(temp)` → `TEMP_SAVING` → result must be SUCCESS and
  `config()==temp` → `requestSave(original)` → `RESTORING` → result must be
  SUCCESS and `config()==original` → evaluate → `DONE` (then re-armed to `IDLE`).
  Once the temp config commits, restoring is mandatory whatever BLE does. If the
  temp save fails, ConfigStore's last-good is kept and no cleanup write is
  invented. Overall bound 60 s (ConfigStore worst case is 6 gate operations, each
  bounded by the gate's 4 s budget).
- **PASS requires all of:** temp and restore verified; `Periph.connected()==1`
  at the end and `ble_disconnect_events` unchanged since start; gate config
  deltas `async_accepted > 0`, `completions_success == async_accepted`,
  `completions_error == timeouts == late_completions == 0`. Expected count with
  the current code is 6 (two saves × erase + body program + commit program; the
  count is reported, not hard-required). A BLE disconnect fails the coexistence
  result but never prevents the restore.
- **Output** (no per-tick spam): one `FLASH PROBE START ...`, then one of
  `FLASH PROBE PASS temp_verified=yes restore_verified=yes ble_connected=yes
  ble_disconnects=0 async_accepted_delta=N completions_success_delta=N
  errors_delta=0 timeouts_delta=0 late_delta=0` or `FLASH PROBE FAIL[ restore]
  stage=<temp-save|temp-verify|restore-request|restore-save|restore-verify|
  timeout|ble|async-evidence> ...` (a restore failure also prints current and
  original config values).
- **Physical result — PASS for the shared ConfigStore async path.** On real
  RAK4631 hardware at branch head `378ba6a7`, the probe image was uploaded,
  one Samsung/nRF Connect peripheral connection was held
  (`BLE ready=yes advertising=no connected=1 policy=open initial_start=ok`),
  and `FLASH PROBE` produced:
  `FLASH PROBE START orig_interval=180 orig_mah=0 temp_mah=1 ble_connected=1 disconnect_snapshot=0 async_accepted=0 stale_result=none`;
  then
  `FLASH PROBE PASS temp_verified=yes restore_verified=yes ble_connected=yes ble_disconnects=0 async_accepted_delta=6 completions_success_delta=6 errors_delta=0 timeouts_delta=0 late_delta=0`.
  This physically exercises ConfigStore→gate→SoftDevice completion forwarding
  while BLE remains connected: both saves completed (6/6 accepted operations),
  the temporary value was verified, the exact original config was restored,
  and no BLE disconnect/error/timeout/late completion occurred. It does **not**
  separately prove HistoryStore or SecurityStore client-specific mutation paths.
  After the run, the normal production `rak4630` image from the same branch was
  re-flashed successfully (`Device programmed`, `rak4630 SUCCESS`).
- **Known limits.** Host tests cover the state machine only. A `restore-verify`
  failure is a defensive check that the real ConfigStore cannot produce (its
  `finishSave()` sets `config_` from the saved candidate), so it is not
  host-reachable. If the 60 s bound ever fired while a save was still in flight,
  the report prints the then-current config and flags a restore failure when it
  differs from the original.

## 9. Physical-validation evidence

**Host/build PASS is not physical PASS.** Current production-path lifecycle,
LoRa coexistence and stock-bond evidence below was collected on one real RAK4631
running `9b7379e` (the §8.5 direct-event implementation) with a Samsung phone
running Nordic nRF Connect for Mobile and USB serial. Commit `378ba6a7` adds
only the macro-guarded test probe/test/docs; its normal `rak4630` production
build remains 22,084 B RAM / 225,484 B Flash and does not contain the probe.
The flash-concurrency probe itself was physically run from the dedicated
`rak4630_m7p7b_flash_probe` image at `378ba6a7`, then the production
`rak4630` image was restored successfully.

Legend: PASS = observed on hardware; PENDING = not yet performed (not a
failure); BLOCKED = cannot be performed on this physical unit as configured
(not PASS, not N/A for the product); N/A = genuinely not applicable to this
slice (a reachable-but-untested path is PENDING, never N/A).

**M7P7B merge evidence is complete with explicit owner dispositions, not with
universal physical PASS.** Quantitative current/power remains unmeasured and is
deferred because no measurement equipment is available. GNSS coexistence remains
blocked on this unit (`GNSS: not detected`) and is owner-waived for this
milestone merge only; it must be re-verified later on a GNSS-equipped unit.
The deliberately between-loop-polls lifecycle timing and injected advertising
start/stop failures remain host-only and are accepted as such for M7P7B.

### 9.0 Current production-path evidence

- **Direct event lifecycle — PASS.** On `9b7379e`, phone connect gave
  `advertising=no connected=1`; disconnect produced
  `BLE advertising restarted`, then `advertising=yes connected=0 policy=open`;
  reconnect within that fresh window returned
  `advertising=no connected=1 policy=open`. This re-proves the lifecycle after
  the §8.5 event-path change. The exact "entirely between two loop polls" phase
  remains host-tested only.
- **BLE/SoftDevice + real LoRa coexistence — PASS (scoped).** With the BLE
  runtime active, real direct LoRa POSITION reception was observed
  (`BASE RX NEW`). While a BLE client was connected, the unit was switched to
  RELAY and real `RELAY RX → RELAY QUEUE → RELAY TX → RELAY TX done` cycles
  were observed, including sequence 11531; additional relay cycles were also
  observed during the same session. `TX done` proves local SX1262 transmission
  completion only; it is **not** proof that a BASE or final recipient received
  the relayed packet.
- **Stock framework bond persistence — PASS (scoped).** nRF Connect created a
  stock Bluefruit/SoftDevice bond; after a full power-off (~5 s) and reboot the
  phone still showed `BONDED`, and reconnect succeeded without a new pairing
  prompt. This is practical evidence that the relocated InternalFS bond path
  persists/reloads across reboot. It is not a raw-flash byte audit and a stock
  BLE bond is **not** ORUN ownership or authorization.
- **BLE + real ConfigStore flash mutation — PASS (shared-path scope).** The
  §8.6 probe at `378ba6a7` completed 6/6 async flash operations with the phone
  remaining connected, verified the temporary config and exact restoration, and
  reported zero disconnects/errors/timeouts/late completions. This physically
  proves the ConfigStore→FlashMutationGate→SoftDevice event bridge under active
  BLE; HistoryStore and SecurityStore client-specific mutation paths were not
  separately exercised on hardware.
- **Production restore — PASS.** After the probe, the normal
  `pio run -d firmware -e rak4630 -t upload` completed with
  `Device programmed` / `rak4630 SUCCESS`.

### 9.1 Consolidated physical checklist

| # | Item | Status | Evidence / reason |
| --- | --- | --- | --- |
| 1 | Real RAK4631 boots with BLE and advertises | **PASS** | Real boot advertising and `BLE ready=yes advertising=yes connected=0 policy=open initial_start=ok` observed; current production code path is unchanged by the probe commit. |
| 1b | Clean full power-cycle / cold boot recovery | **PASS (scoped)** | Full power-off/power-on followed by healthy BLE operation was observed. This does not claim the device was connected at the instant power was removed. |
| 2 | Real RF scanner sees the peripheral | **PASS** | Samsung/nRF Connect saw `ORUN-4B275BA5`, address `C5:DD:01:85:C9:4B`. |
| 2b | Phone connects | **PASS** | Serial: `BLE ready=yes advertising=no connected=1 policy=open initial_start=ok`; only standard GATT `0x1800`/`0x1801`, no ORUN application GATT. |
| 3 | Connected client survives the original ~10-min no-client deadline | **PASS (earlier production-path evidence)** | Phone remained connected well beyond ~10 minutes. No exact duration beyond that is claimed. |
| 4 | Disconnect → loop-owned restart → advertising restored | **PASS on §8.5 path** | Current direct-event build logged `BLE advertising restarted` and then `advertising=yes connected=0 policy=open`. |
| 4b | Fresh post-disconnect admission window | **PASS on §8.5 path** | Reconnect succeeded during the renewed window. |
| 5 | BLE closes after a full window with no client | **PASS** | Current logs include `BLE closed; no client connected within window`. |
| 6 | Reconnect during the renewed window | **PASS on §8.5 path** | Reconnect gave `advertising=no connected=1 policy=open`. |
| 7a | LoRa TX/RX coexistence with SoftDevice/BLE active | **PASS (scoped)** | Direct RX was observed with BLE runtime active; with a BLE client connected, real RELAY RX/QUEUE/TX/TX_DONE was observed. Local TX completion is not end-to-end delivery. |
| 7b | GNSS coexistence | **BLOCKED (this unit); OWNER-WAIVED FOR M7P7B MERGE** | Unit reports `GNSS: not detected`. Not PASS/N/A. Physical re-validation remains required on a GNSS-equipped unit; this missing evidence no longer blocks this milestone merge by explicit owner decision. |
| 8 | Flash mutation concurrency with BLE active | **PASS for ConfigStore/shared gate path; History/Security not separately exercised** | §8.6 physical probe: temp+restore verified, BLE stayed connected, 6 accepted / 6 successful async completions, 0 errors/timeouts/late completions/disconnects. |
| 9 | Very short connect+disconnect entirely between loop polls | **HOST-ONLY ACCEPTED FOR M7P7B** | Direct-event policy/startup tests cover this timing; phone testing cannot prove the loop phase. Not physical PASS. |
| 9b | `Advertising.start()`/`stop()` failure and retry | **HOST-ONLY ACCEPTED FOR M7P7B** | No practical physical fault injection used; retry/race behavior is host-modelled and mutation-tested. Not physical PASS. |
| 10 | Stock bond creation/persistence via relocated `InternalFS` | **PASS (scoped)** | Bond created, full power-cycle performed, phone remained BONDED and reconnected without a new pairing prompt. Framework bond ≠ ORUN authorization. |
| 11 | Current/power measurement (advertising / connected / closed) | **DEFERRED — OWNER ACCEPTED NON-BLOCKER** | No current-measurement equipment is available. Quantitative current is not measured; the power-bank endurance observation is not a calibrated current measurement and is not recorded as PASS. |

No checklist row is N/A. The remaining physical unknowns are explicit rather
than converted into software/build claims.

### 9.2 Historical evidence on `65893e2` (superseded by §9.1)

Collected with a Debian PC (BlueZ) as the only client, on the pre-audit
firmware where advertising restart after disconnect was framework-owned.
Retained for history only; it is not evidence for the current build.

- PC/BlueZ: advertising as `ORUN-4B275BA5` (matching same-boot `BLE?`), a
  connection succeeded, connected state outlived the ~10-min deadline,
  advertising/fresh window restored after disconnect, no-client window closed
  normally.
- Reconnect via BlueZ showed transient `[NEW]`/`[DEL]`, `Device not available`
  and `le-connection-abort-by-local` while firmware simultaneously reported
  `advertising=yes connected=0 policy=open`. This is recorded as an
  **inconclusive tooling/client (BlueZ) issue, not a firmware FAIL**: BlueZ
  later saw the device, and the phone scanned, connected and reconnected on the
  current firmware (§9.1). No FAIL is recorded against the firmware.
- Post-re-upload `BLE?` recovery (not a clean power-cycle) — superseded by row 1b.

Summary: on `9673f49`, rows 1, 1b, 2, 2b, 3, 4, 4b, 5, 6 are PASS on one unit
with one phone/client type; rows 4, 4b and 6 must be re-run on the §8.5 build
(§9.0). Rows 7a, 8, 9, 9b, 10, 11 are PENDING; 7b is BLOCKED on this unit. No
row is N/A. Do not merge on the basis of this evidence alone (§11).

## 10. Explicit non-claims / deferred work

Recorded, not hidden:

- LoRa `OPEN_BLE` command — requires the secure downlink/authenticated command
  path, which does not exist yet. Deferred, not implemented, not stubbed with
  an unauthenticated shortcut.
- Any ORUN-specific application GATT service (provisioning, config, diagnostics, DFU).
- Multi-client BLE.
- Role-differentiated BLE availability (BASE/MOBILE "continuous" per
  `AGENTS.md`) — this slice applies the same tracker admission policy to every
  role uniformly; role-aware BLE policy is future work.
- A stalled-session watchdog for a connected client making no progress
  (`ORUN_FIELD_NETWORK_DIAGNOSTICS_PLAN.md` §10 already flags this as a
  distinct future requirement, not part of this slice).
- Bonding/pairing UX, first-phone ownership, PIN/passkey, or any authorization
  model tied to BLE connection/bonding. Stock framework pairing/bonding was
  exercised only to validate persistence/reload (§9); it is neither an ORUN
  ownership/authorization mechanism nor a designed product provisioning UX.

## 11. Merge disposition

`AGENTS.md` requires that anything still needing physical hardware testing be
identified and that compilation/build success not be presented as physical proof.
This section records the explicit owner disposition for the remaining evidence
gaps; it does not convert any deferred or blocked item into PASS.

**Physical gates closed by real evidence:**

- §8.5 post-disconnect lifecycle: disconnect → loop-owned advertising restart →
  fresh window → reconnect — **PASS**.
- Real stock bond creation plus reboot persistence/reconnect through relocated
  InternalFS — **PASS (scoped)**; this is not ORUN authorization.
- Real LoRa coexistence with active BLE: direct RX plus BLE-connected relay
  RX/QUEUE/TX/TX_DONE — **PASS (scoped)**; TX_DONE is local completion only.
- Real ORUN flash mutation through the shared ConfigStore →
  FlashMutationGate → SoftDevice completion path while BLE remains connected —
  **PASS** (§8.6). HistoryStore/SecurityStore client-specific mutation paths are
  not separately claimed as physically exercised.

**Explicit owner dispositions for evidence that cannot currently be produced:**

- Quantitative current/power measurement is **DEFERRED** because no measurement
  equipment is available. It is **not PASS**, but it is accepted as a
  non-blocker for M7P7B. Future power work must still measure advertising,
  connected and post-close states on hardware.
- GNSS coexistence remains **BLOCKED on this unit** because it reports
  `GNSS: not detected`. The owner explicitly waives this as an M7P7B merge
  blocker only. It remains an open physical-validation obligation for a
  GNSS-equipped unit and must not be described as PASS or N/A.
- Very short connect+disconnect entirely between loop polls and injected
  `Advertising.start()`/`stop()` failure/retry remain **host-only**. Their
  current deterministic/mutation-tested host coverage is accepted for this
  milestone; no physical PASS is claimed.

**Deferred / out of scope for M7P7B:**

- ORUN authorization, ownership, provisioning, PIN, application GATT services,
  LoRa `OPEN_BLE`, DFU/M7P8, secure envelope, multi-client BLE, role-aware BLE
  policy, stalled-session watchdog (§10).

The final static audit at `69bfd005` found no new blocking firmware correctness
defect. The subsequent owner-disposition commit is documentation-only. With the
power and GNSS decisions above, **no known M7P7B-specific merge blocker remains**.

