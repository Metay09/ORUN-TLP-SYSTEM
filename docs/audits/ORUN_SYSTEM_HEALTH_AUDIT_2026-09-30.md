# ORUN-TLP — Full Project / Product / Architecture Health Check

Status: **INDEPENDENT AUDIT — READ-ONLY. No production source, test, fixture,
storage layout or branch was modified.**

Date: 2026-09-30
Auditor: Claude (Opus 5.5), Claude Code session, owner-requested.
Canonical baseline: `origin/main@b4d97d9a02923e68aaf4806c344a129825fce4e3`
("M6D3C: activate durable geofence runtime provider (#56)").

Evidence discipline used in this document:

- **MEASURED** — produced in this audit on the exact baseline (build, symbol
  table, `-fstack-usage`, host test run, host probe).
- **CODE** — read directly from the baseline source with file/line reference.
- **DOC** — taken from an existing repository milestone/audit record.
- **ESTIMATE** — calculated or bounded from datasheet-class numbers; never a
  physical measurement. Every ESTIMATE must be replaced by bench measurement
  before it drives a behavior change.

Host PASS and build PASS are never reported here as physical PASS.

---

## 1. Executive summary

ORUN's trunk is in better shape than most prototypes of this size. The
ownership model is explicit (single FlashMutationGate, single radio driver
gate, loop-owned stores, generation-tagged callbacks), there is no heap, STL,
RTTI or exceptions in ORUN code, the persistence layer is A/B commit-last with
explicit UNCERTAIN/maintenance semantics, TLP v1 golden fixtures are intact and
the full host suite (warnings-as-errors + ASan/UBSan, 67 PASS lines, 11
production-startup scenarios) passes on the baseline. RAM (11.6 %) and flash
(31.9 %) headroom are large.

The weaknesses are not "architecture purity" problems. They are **field-product
recovery and energy problems** that the current bench-driven milestones have
not yet had to face:

1. **Reboots silently destroy local history** (MEASURED). Every boot reserves a
   new sequence block; every 8th boot erases the oldest history page. With no
   new data, 56 clean reboots erased 600 of 600 records in a host probe using
   the unchanged `HistoryStore`. A brown-out or watchdog loop on a depleted
   solar collar is exactly the situation where history matters most.
2. **Role is still inferred from GNSS presence and is not persisted.** A single
   failed GNSS detection at boot turns an animal tracker into a BASE with
   continuous SX1262 RX and no tracking, with no retry until reboot; a RELAY
   loses forwarding after every reset because the USB override is volatile.
3. **One flash failure disables POSITION TX for the rest of the boot**
   (documented, deliberate fail-stop) while GNSS keeps spending energy on
   fixes that are then dropped.
4. **There is no battery measurement anywhere in firmware.** "What is battery
   state?" cannot be answered, and there is no staged low-battery protection.
5. **GNSS failure costs more energy than success.** An obstructed/indoor
   acquisition runs the full 120 s every interval with no backoff (ESTIMATE:
   10–40× the energy of a warm fix, zero positions), and a timeout always cuts
   the GNSS rail even in continuous mode — losing exactly the lock retention
   the recent field observation says is valuable.
6. **Store-and-forward is store-only.** Records are persisted but never
   replayed or acknowledged; `delivered_through` never advances. Capacity is
   728 records ≈ 36 h at the 180 s development default, 12 h while OUTSIDE.
7. **RF capacity is dominated by relay duplication and SF11 airtime.** With the
   180 s development cadence, a single always-forwarding relay exceeds a 10 %
   duty-cycle budget at ~15 trackers (~5 trackers while all are OUTSIDE at
   B/3). There is no airtime accounting or enforcement in firmware.

None of these is a BLOCKER for the current development scope, and none requires
a large refactor. Several are small, testable changes; the rest need owner
decisions backed by bench energy measurement.

**Strategy Pattern:** not justified anywhere today as a runtime virtual-dispatch
framework. Two *small pure-policy extractions* are justified (GNSS power/backoff
decision; effective-cadence resolution already exists). See §16.

**Verdict: HEALTHY WITH TARGETED FIXES** (details in §A).

---

## 2. Canonical repo baseline / exact SHA

| Item | Value |
| --- | --- |
| `origin/main` after `git fetch` | `b4d97d9a02923e68aaf4806c344a129825fce4e3` (matches the SHA given in the request) |
| Commit | `M6D3C: activate durable geofence runtime provider (#56)`, 2026-09-30 10:48 +0300 |
| Local checkout | branch `feat/m6d3c-geofence-runtime-provider`, 9 commits behind its remote; one untracked file `docs/audits/M6D3B_FINAL_PRE_PHYSICAL_AUDIT.md`. **Not used as audit input and not touched.** |
| Audit method | Detached worktree of `b4d97d9` in a session scratch directory; builds and probes ran there only. |
| Production build (MEASURED) | `pio run -e rak4630`: SUCCESS. **RAM 28,744 / 248,832 B (11.6 %)**, **Flash 260,008 / 815,104 B (31.9 %)** — identical to the M6D3C record. |
| Host suite (MEASURED) | `firmware/tests/run_host_tests.sh`: **EXIT 0**, 67 `PASS` lines incl. 11 production-startup scenarios; wall time 8 min 31 s. |
| Toolchain | PlatformIO nordicnrf52@11.0.0, Adafruit nRF52 core 1.7.0, GCC ARM 7.2.1, SX126x-Arduino 2.0.32, SparkFun u-blox GNSS 2.2.29, TinyUSB 3.6.0. |

Sources read: `AGENTS.md`; all of `docs/architecture/*` headings and the
governing documents in full (README, PRODUCT_SYSTEM_ARCHITECTURE, GEOFENCE
policy, relevant sections of security/persistence/downlink/diagnostics
plans); milestones M1–M6D3C, M7P6x/M7P7x as needed; audits R1–R4, M6D3B,
M6D3C; every file in `firmware/src` and `firmware/include` that participates
in production composition; `platformio.ini`; storage/patch scripts; the host
test runner, startup harness and source-contract tests.

---

## 3. Product completion map

Legend: IMPL = implemented in production image; PART = partial; DESIGN =
contract/design only; TEST = test-only image/probe; PHYS = physically
qualified (scope-limited, per the cited record); SW = software-only verified;
NONE = not implemented; DEFER = explicitly deferred.

| Subsystem | Status | Evidence / boundary |
| --- | --- | --- |
| Device identity (64-bit nRF FICR, RAK byte order) | IMPL, PHYS | B1A fixtures; B4 mixed-fleet direct path PASS (README §3). |
| Location (source-neutral owner, last-known, freshness) | **PART** | Only a GNSS fix path exists (`GnssFix`, 5 s live freshness). No Location owner, no last-known value, no second source; `RequestedLocationSource` = `kNone`/`kGnss` only. |
| GNSS acquisition / freshness / I2C recovery | IMPL, PHYS (functional) | M2/M3/R3/R4, M6P1 timing. **Quantitative power DEFERRED**; continuous-GNSS-at-60 s power **not** validated (M6D2 §253). |
| Activity (accelerometer) | PART | M6A probe/shutdown PHYS; M6B3 manual USB `ACTIVITY START` capture PHYS. No continuous sampling, no classification, no LoRa activity data. |
| Radio TX/RX (SX1262 P2P) | IMPL, PHYS | M1/M2/R2; M6P1 windowed tracker RX functional PASS. |
| Relay (TLP v1 one hop) | IMPL, SW (+ carry-forward) | M5 host; relay path **not re-run physically since M6P1** (M6P1 §8). |
| Gateway direction (BASE→phone/backend, downlink) | DESIGN | BASE only prints Serial lines. Downlink rendezvous & gateway authority are plans. |
| HistoryStore (store-before-send ring) | IMPL, SW | 728-record ring; power-cut coverage is host fault-injection. **Physical brown-out/power-removal still pending** (M4 §105, R1 §102). |
| Store-and-forward replay / delivery ACK | **NONE** | `getNextBacklog`, `markDeliveredThrough`, `saveReplayCursor` have no production caller (CODE grep). |
| ConfigStore v2 | IMPL, PHYS (scoped) | CONFIG_STORE_V2_PHYSICAL_QUALIFICATION. No transport writer. Durable B applied at boot only. |
| SecurityStore v2 | IMPL (recovery-only), PHYS (partial-erase sentinel) | No provisioning, so production devices stay UNPROVISIONED and write nothing. |
| GeofenceStore | IMPL, PHYS (M6D3B) | No production writer; **not polled in `loop()`** (§4 F-L1). |
| Geofence runtime (M6D2 confirmation, B/3 cadence) | IMPL, PHYS (M6D3C activation + ~60 s cadence observed) | Focused M6D2 INSIDE→OUTSIDE→INSIDE physical qualification still DEFERRED (M6D2 §309). |
| OUTSIDE event transport | NONE | Local Serial line only; secure EVENT/v2 required. |
| LOST semantics | DESIGN | Gated on authenticated contact/receipt. |
| BLE transport (GATT, framing, session) | IMPL, PHYS | M7P7G Android/nRF Connect. |
| BLE product surface | **PART** | One read-only request: GET_CONFIG. No device/location/GNSS/geofence/power/radio/storage/health status. |
| Mobile app | NONE | — |
| Backend | NONE | — |
| Secure commands / secure envelope | DESIGN + TEST | CC310 HKDF/AES-CCM KAT and Bluefruit coexistence are test-only probes (M7P6C/E). |
| Events / alarms | NONE | — |
| Configuration distribution (DEVICE/GROUP/FLEET) | DESIGN | — |
| Firmware update / DFU | PART (bench only) | USB serial/UF2 only; bootloader signing/bank behavior **UNKNOWN** (ADR M7P6 §12); BLE DFU NONE. |
| Diagnostics | PART | USB text: `RADIO?`, `BLE?`, `ACCEL?`, `ACTIVITY?`, `APP CONFIG?`, `ROLE?`. GNSS/History/Security/Gate counters exist in RAM but are **not queryable**. |
| Telemetry / health | NONE | No battery, temperature, uptime, reset or health packet. |
| Power management | PART | Sensor-rail ownership, GNSS rail release, windowed tracker RX, FreeRTOS tickless idle via `delay(10)`. **No battery measurement, no low-battery staging, no quantitative measurement.** |

---

## 4. Architecture health map

Severity scale as requested. Only findings with a concrete failure scenario or
measured cost are listed.

### HIGH

**F-H1 — Reboot-driven history erasure (MEASURED).**
`HistoryStore::begin()` always calls `startReservation()`
(`firmware/src/history_store.cpp:26`). Each page has 8 sequence slots; when
they are used, the reservation calls `startNewPage(false)`
(`history_store.cpp:106`), which erases the next ring page — the oldest data.
Host probe (unchanged production `HistoryStore`, synchronous NOR model, 600
stored records, then clean reboots with **no new appends**):

```text
boot=1..7   records=600
boot=16     records=496 overwritten=104
boot=32     records=288
boot=48     records=80
boot=56     records=0
```

Failure scenario: a solar collar at low battery brown-out-loops (boots,
GNSS/TX current dips the rail, resets). Each ~8 boots destroys ~104 records;
56 boots destroy the full history — while nothing new is being recorded.
Watchdog loops have the same effect. Sequence non-reuse is preserved; data is
not.

**F-H2 — Role inferred from GNSS presence, override volatile.**
`RoleController::updateAutomatic()` maps "GNSS not detected" to BASE
(`firmware/src/node_role.cpp:45`); GNSS detection has 3 boot attempts and then
stays `kNotPresent` until reboot (`gnss_config.h` `kDetectionMaxAttempts`).
BASE ⇒ `receive_application_position` ⇒ continuous RX. USB `ROLE` override is
volatile (DOC: `ORUN_CURRENT_ARCHITECTURE_RULES.md:121`).
Failure scenarios: (a) a transient I2C/GNSS detection failure on an animal
collar yields a silent BASE: no tracking, continuous RX (ESTIMATE ~5 mA),
battery exhausted in days; (b) a GNSS-less RELAY reverts to BASE (forwarding
OFF) after any reset including watchdog — relay coverage silently disappears.
This is the "role decides everything" coupling the architecture documents
already warn about; it is now the largest *field-recovery* gap.

**F-H3 — HistoryStore fail-stop disables POSITION TX until reboot.**
`HistoryStore::fail()` sets `ready_ = false` (`history_store.cpp:145`); nothing
but `begin()` sets it back. `PositionFlow` then drops every fix
("STORAGE position dropped; no live TX"). This is deliberate (DOC:
`M4_FLASH_JOURNAL.md:33`) and correct for store-first integrity, but for a field
product there is no bounded recovery, no health surface and GNSS keeps
acquiring (energy with zero output). With SoftDevice enabled after BLE start,
every history write goes through the async gate with a 4 s operation timeout
and a 6 s token wait — an InternalFS/BLE contention or a SoftDevice flash
ERROR event is enough to trigger it.

**F-H4 — No battery measurement or low-battery policy.**
No ADC/VBAT code exists (CODE grep: no `analogRead`, no VBAT pin in
`rakwireless/variants/rak4630/variant.h`). `battery_capacity_mah` in
ConfigStore is metadata only. AGENTS.md requires staged low-battery protection
for the ANIMAL_TRACKER preset. Battery state is one of the product's root
questions and a prerequisite for handling F-H1 (brown-out loops) sensibly.

**F-H5 — GNSS failure mode is the most expensive mode.**
`GnssManager::poll()` times out after 120 s, sets `needs_configuration_ = true`
(`gnss_manager.cpp:123`); `enterLowPower()` then always releases the rail
because `keepTracking()` requires `!needs_configuration_`
(`gnss_manager.cpp:440-469`). There is no failure backoff: the next attempt
starts at the next grid point. ESTIMATE (ZOE-M8Q-class ~25 mA acquisition,
not measured):

| Outcome per attempt | GNSS on-time | Charge | Useful position |
| --- | ---: | ---: | --- |
| Warm reacquire after rail cut (DOC M6P1: 3–13 s TTFF incl. 1 s settle) | 3–13 s | ~75–325 mA·s | yes |
| Continuous tracking, one fix per 60 s | 60 s | ~1,400 mA·s | yes (lock retained) |
| Timeout (indoor/obstructed) | 120 s | ~3,000 mA·s | **no** |

At B = 180 s an obstructed collar spends ≈ 67 % GNSS duty (~17 mA average,
ESTIMATE) and produces nothing. In continuous mode (B/3 = 60 s OUTSIDE) a
single timeout power-cycles the receiver, discarding the lock-retention the
recent field observation shows can keep indoor tracking alive.

### MEDIUM

**F-M1 — `main.cpp` is a 2,060-line composition root that also owns runtime
state machines.** It holds the geofence episode buffers and slot-0 dedupe
(`geofence_episode_fixes`, `geofence_slot0_normal_store_*`,
`geofence_representative_*`), the complete BLE application session/indication/
disconnect-recovery runtime (~10 loop-owned globals), the USB text parser and
~560 lines of `#ifdef` test-probe code (M7P6E ~290, M7P7B ~175, M6D2 ~95).
Those runtimes are tested only through the stubbed startup harness and
text-matching source-contract tests. Concrete risk: the next three committed
slices (read-only status, geofence writer, LOST) will all land in this file.

**F-M2 — History capacity and priority vs. product target.** 7 pages × 104
records = 728 (`storage_config.h`). Coverage: 36.4 h at B = 180 s, 12.1 h at
60 s (OUTSIDE), 7.6 d at 15 min, 15.2 d at 30 min. AGENTS.md targets 1–2 weeks.
Rollover silently overwrites undelivered records; there is no critical-event
record class that outranks routine positions.

**F-M3 — O(N) flash scans on the hot path.** `append()` calls `newest()`
(full scan, `history_store.cpp:81/87`) and `main.cpp:2001` prints
`backlogCount()` (another full scan) after every stored fix; boot does a third.
Each scan decodes every record with bitwise CRC32 and a
serialize/deserialize round trip. MEASURED host cost: 718 µs per 600-record
scan (x86 -O2). ESTIMATE on Cortex-M4F @ 64 MHz: ~10–30 ms per scan, i.e.
~20–60 ms loop blocking per fix. Small energy, but it scales linearly with any
future history enlargement (F-M2) and blocks radio/BLE servicing.

**F-M4 — TLP v1 has no network/domain identity and no sequence epoch.** A relay
forwards any structurally valid POSITION on 869.525 MHz / sync 0x1424
(`network_service.cpp` `receive()`), including a neighbouring installation's
trackers or spoofed frames, consuming 1.23 s of relay airtime each. The 32-bit
wire sequence restarts if the history partition is ever re-initialized, and a
backend cannot distinguish that from replay. Frozen for v1 (correctly); must be
a v2 trunk input.

**F-M5 — No airtime/duty-cycle accounting or enforcement.** Airtime estimator
exists only in tests (`lora_airtime.h`, used by `tests/m5`). §10 shows a single
relay passing 10 % duty at ~15 trackers (B = 180 s). The 10 % figure assumes
the EU863-870 869.4–869.65 MHz sub-band; AGENTS.md forbids silently
hard-coding regulatory assumptions, but *not having any budget* is equally
silent.

**F-M6 — Stack margin not observable in production.** Loop task stack is
4,096 B (`LOOP_STACK_SZ 256*4` words, framework `main.cpp:42`). MEASURED
`-fstack-usage` frames on the boot chain: `GeofenceStore::recover` 656 B →
`geofence_format::inspectPage` 592 B → `decodeBody` 600 B (≈ 1.85 KB before
`setup()`/printf frames). M6D3B called this "tight-but-safe" in its
qualification image; production has no high-water-mark diagnostic.

**F-M7 — Open BLE application GATT.** Request WRITE and response INDICATE use
`SECMODE_OPEN` (`main.cpp:312/322`). Acceptable for GET_CONFIG (interval,
battery capacity). **Not acceptable** for the planned read-only
location/geofence status: "read-only" is not "public"; exposing live location
to any phone in range during the 10-minute window is a privacy defect.

**F-M8 — Tracker 10 s post-TX RX window has no consumer yet.**
`kWindowedRxAfterTxMs = 10000` (`radio_config.h:37`). No downlink frame exists
that a TRACKER acts on (it classifies POSITION as `kIgnoredPosition`).
ESTIMATE: 10 s × ~5 mA ≈ 50 mA·s per TX, about the same as the TX itself
(0.987 s × ~45 mA ≈ 44 mA·s). Deliberate M6P2 rendezvous reservation →
**NEEDS DECISION**, not a defect.

**F-M9 — 100 Hz loop wake regardless of work.** `PowerManager::idle()` is
`delay(10)` (`power_manager.h:14`), so the MCU leaves tickless idle ~100×/s to
run the whole loop (radio gate + dispatch, BLE critical sections, flash pump,
GNSS/accel polls). ESTIMATE: tens of µA average — potentially larger than the
SX1262-sleep + GNSS-off floor between fixes. **Unmeasured.**

**F-M10 — B/3 at the 60 s threshold flips GNSS into continuous mode.**
`keepTracking()` uses `interval_ms <= kShortIntervalThresholdMs` (60 s,
`gnss_config.h:25/46`). With the 180 s dev default, OUTSIDE ⇒ 60 s ⇒ GNSS
continuously powered with `powerSaveMode(false)` (`gnss_manager.cpp:279`).
B = 183 s ⇒ 61 s ⇒ power-cycled. Intentional per M6D2 §126-128, never
power-measured. Continuous mode is probably *better* for indoor/obstructed
animals (lock retention) and *worse* in open sky (warm reacquire ≈ 5–20 % of
continuous energy). This is a policy decision that needs measured energy per
useful position, not a code fix. (See §9.)

**F-M11 — Diagnostics exist but are not a product surface.** Counters in
`GnssManager::Diagnostics`, `HistoryStore::Diagnostics`, gate diagnostics,
`SecurityStore::Diagnostics`, geofence token/resource state are not queryable
(only printed on events). The M6D3C follow-up (read-only Device Service) is the
right next slice; it needs a typed status model owned outside `main.cpp`.

### LOW

- **F-L1** `geofence_store.poll()` is not called in `loop()` (`main.cpp:1977-1981`
  polls history/config/security only). Harmless today (no mutation). It becomes
  a **BLOCKER for the first geofence writer slice**: mutations and post-fault
  reconciliation would never advance.
- **F-L2** Four sibling `Nrf*Flash` backends and four near-identical gate client
  paths (~300 lines). Intentional (documented "sibling, not HAL"). Keep.
- **F-L3** RX event queue 4 × 264 B for ≤ 49 B TLP frames (`radio_manager.cpp:30-31`).
  ~820 B RAM reducible, but v2 frame size is unfrozen. Keep.
- **F-L4** `architecture/README.md` §3 is stale (M6C2-era RAM/flash, "coordinator
  unconfigured", "M6B/M6C helpers not referenced by main.cpp"); contradicts
  M6D3C and AGENTS.md.
- **F-L5** Unused vendor board files `rakwireless/boards/{rak11200,rak11300,rak3112}.json`
  and variants since the M0 commit; not referenced by any env.
- **F-L6** Source-contract tests partly pin implementation text (variable
  declarations, log strings) rather than architecture (see §14).

### INFO (healthy)

- No heap allocation, STL, RTTI or exceptions in ORUN code (CODE grep);
  `virtual` only on test seams (`FlashBackend`, `SequenceSource`, incarnation
  sources).
- Unused mutation APIs (`requestSave/Reset`, `requestReplace/Clear`,
  `commitCredential`, replay APIs) are linker-GC'd: 0 B flash (MEASURED `nm`).
- Radio ownership (driver gate + generation + role epoch + stale counters),
  FlashMutationGate quarantine semantics and BLE generation-tagged handoff are
  well-designed and well-tested.
- Framework patches are version-pinned and fail closed.

---

## 5. Top risks (ranked by product value × likelihood)

| # | Risk | Finding | Why it matters first |
| ---: | --- | --- | --- |
| 1 | History destroyed by reboot loops | F-H1 | Data loss exactly when power is marginal; silent. |
| 2 | Tracker silently becomes BASE / relay loses forwarding after reset | F-H2 | No field recovery without USB service; battery drain. |
| 3 | Unknown battery state; no low-battery staging | F-H4 | Root product question; drives F-H1/F-H5 mitigation. |
| 4 | GNSS obstructed-mode energy | F-H5, F-M10 | Dominant energy term; unmeasured. |
| 5 | Storage fail-stop kills TX until reboot | F-H3 | Single fault → silent device. |
| 6 | Store-only history; small capacity | §3, F-M2 | Outage data never delivered. |
| 7 | Relay duty-cycle/airtime unmanaged at 30–50 nodes | F-M5, §10 | Regulatory + collision collapse at dev cadence. |
| 8 | Location over open GATT in next slice | F-M7 | Privacy defect if shipped as "read-only". |
| 9 | `main.cpp` growth | F-M1 | Every committed next feature lands there. |
| 10 | Stack margin invisible | F-M6 | Boot chain already near 2 KB of 4 KB. |

---

## 6. Duplication / wasted-work findings

| # | Where | What repeats | Intentional? | Quantified impact | Keep? |
| ---: | --- | --- | --- | --- | --- |
| D1 | `history_store.cpp:26,106` | Sequence reservation write every boot; page erase every 8 boots | Partly (no-reuse), but erasing data is waste | MEASURED 104 records lost / 8 boots | **Fix** (F-H1) |
| D2 | `history_store.cpp:81,84,87`, `main.cpp:2001` | Full-history decode scans per append and per log line | No | 2 scans × ~10–30 ms per fix (ESTIMATE) | **Fix**: cache newest identity + backlog count |
| D3 | `gnss_manager.cpp:258-333` | 5-step receiver configuration + 1 s settle after every rail cut | Yes — RAM config lost when rail is cut | ~1 s × ~25 mA per power-cycled fix = up to ~⅓ of a warm fix (ESTIMATE) | Keep; *measure* settle time; consider BBR/backup |
| D4 | `gnss_manager.cpp:540-556` | First PVT epoch after drain discarded | Yes — R3 freshness boundary | ~1 s per fix | **Keep** (safety) |
| D5 | GNSS timeout path | 120 s acquisition repeated each interval without backoff | No | up to ~3,000 mA·s per failed attempt (ESTIMATE) | **Fix via policy** (F-H5) |
| D6 | `radio_config.h:37` | 10 s RX after every tracker TX | Reservation for future downlink | ~50 mA·s per TX (ESTIMATE) | NEEDS DECISION (F-M8) |
| D7 | `power_manager.h:14` | Full loop pass 100×/s | Simplicity | tens of µA (ESTIMATE, unmeasured) | Measure first (F-M9) |
| D8 | `network_service.cpp` relay | Relay re-sends packets BASE already heard directly | Yes — path redundancy | +125 % airtime per relayed packet (1.233 s vs 0.987 s) | Keep for v1; v2 policy input |
| D9 | position encode/store/TX | POSITION serialized/deserialized 3–4× (encode, `validPacket`, `encodeRecord`, TX validation) | Yes — defense in depth | < 10 µs each | **Keep** |
| D10 | `flash_mutation_gate.h:410-413` | Gate staging buffers (1,072 B) duplicate client `blob_` buffers | Yes — SoftDevice needs stable source until completion | ~1 KB RAM | **Keep** |
| D11 | Geofence geometry | Copies in `GeofenceStore` (snapshot, pending, blob, scratch, 2 recovery pages, inspection), `GeofenceRuntime::vertices_`, `geofence_boot_snapshot` | Yes — ownership separation + recovery workspaces off the 4 KiB stack | ~5 KB RAM total; only the 524 B boot copy is avoidable | **Keep** (boot copy pinned by source contract; trivial) |
| D12 | `main.cpp:1878` | `resolveRuntimeConfig()` every loop | Simplicity | ~100 cycles | Keep |
| D13 | 4 × `Nrf*Flash`, 4 × gate client paths | Near-identical code | Yes (documented) | ~1–2 KB flash | Keep (F-L2) |
| D14 | `run_host_tests.sh` | Same TUs recompiled per test binary with ASan | Simplicity | 8 min 31 s wall time | LATER: parallelize, do not merge tests |

No duplicated telemetry exists (no telemetry exists). No duplicate RF packet is
generated by one node for one observation: the M6D2 slot-0 guard
(`main.cpp:1636-1645`) correctly prevents a second sequence/history record for
the representative fix.

---

## 7. RAM / Flash / stack / heap report

### 7.1 Budget (MEASURED)

| Resource | Used | Limit | Headroom |
| --- | ---: | ---: | ---: |
| Static RAM (.data 2,952 + .bss 25,792) | 28,744 B | 248,832 B | 220,088 B |
| FreeRTOS/malloc heap region | 206,776 B | — | task stacks come from here |
| Flash (image) | 260,008 B | 815,104 B (0x26000–0xED000) | 555,096 B |
| Flash vs. real app ceiling (`kApplicationPolicyEndAddress` 0xE5000) | 260,008 B | 782,336 B | 522,328 B |
| Flash if a future bootloader requires dual-bank DFU (ESTIMATE ½ ceiling) | 260,008 B | ~391,168 B | **~131 KB** |

The last row is the only resource number that matters strategically: if BLE
OTA ends up dual-bank, the product has ~131 KB of growth left. Decide the DFU
model before adopting large libraries.

### 7.2 Largest RAM consumers (MEASURED `nm`)

| Object | Bytes | Comment |
| --- | ---: | --- |
| InternalFS `_cache_buffer` | 4,096 | Framework bonds (LittleFS). |
| `geofence_store` | 4,000 | Intentional recovery workspaces (M6D3B stack fix). Keep. |
| TinyUSB non-CDC classes + host stack | ~3,175 | MSC/HID×2/MIDI/Vendor/Video/Host enabled by framework config. |
| Bluefruit | 1,940 | Framework. |
| `storage_flash_gate` | 1,404 | 1,072 B staging (D10). Keep. |
| `radio_manager` | 1,240 | Incl. NetworkService dedupe/queue. |
| RX event queue storage | 1,056 | F-L3. |
| Timer task stack / queue | 1,536 | Framework. |
| `history` | 808 | |
| `geofence_confirmation` | 628 | Runtime geometry copy. |
| `geofence_boot_snapshot` | 524 | Boot only (D11). |
| `security_store` | 488 | |
| SparkFun `gnss` object | 392 | |

Task stacks (CODE): loop 4,096 B; SX126x `LORA` task 16,384 B
(`xTaskCreate(..., 4096 words)`); Bluefruit BLE/SOC per framework config.

### 7.3 Flash by component (MEASURED, symbol-grouped, approximate)

| Component | Flash |
| --- | ---: |
| ORUN objects (`src/*.o` text, before GC) | ~85 KB (main 12.4 KB, security_store 10.0, radio 6.8, gate 6.7, gnss 5.6, history 5.3, geofence_store 5.2, config_store 4.6 …) |
| SparkFun u-blox GNSS | ~28.7 KB |
| LittleFS (bonds) | ~20.7 KB |
| SX126x-Arduino | ~18.6 KB |
| CC310/ECC (LESC via Bluefruit) | ~13.9 KB |
| Bluefruit | ~11 KB |
| printf/dtoa/scanf | ~8.9 KB |
| TinyUSB unused classes | ~7.9 KB |

### 7.4 Stack (MEASURED `-fstack-usage`, static frames)

Largest ORUN frames: `GeofenceStore::recover` 656, `decodeBody` 600,
`inspectPage` 592, `GeofenceStore::establishFreshBaseline` 592,
`GeofenceStore::writeBlob`/`startMutation` 584, `NrfGeofenceFlash::program`
576 (560 B aligned copy), `SecurityStore::recover` 432,
`RadioManager::processReceivedEvents` 416 (264 B `RadioRxEvent` local),
`HistoryStore::poll` 384, `NrfHistoryFlash::program` 368 (352 B aligned copy),
`loop` 176.

Representative chains (static sums, excluding printf/framework):

- boot: `setup` → `GeofenceStore::begin` → `recover` 656 → `inspectPage` 592 →
  `decodeBody` 600 ≈ **1.9 KB**;
- steady loop, history write: `loop` 176 → `history.poll` 384 → `writeBlob` 368 →
  gate → `NrfHistoryFlash::program` 368 ≈ **1.3 KB**;
- steady loop, RX: `loop` 176 → `update` → `processReceivedEvents` 416 →
  `handleReceivedPacket` 136 → `NetworkService::receive` 216 ≈ **1.0 KB** +
  `printf`.

Likely margin ≥ 1.5 KB, but **not observable** on production hardware (F-M6).
Radio callback path copies a 264 B event on the 16 KB LORA task stack: fine.

### 7.5 Heap / language features

No `new`/`malloc`/STL/RTTI/exceptions in ORUN code. Templates: only
`PacketDedupeCache<N>` (2 instantiations). Heap fragmentation risk is confined
to framework tasks/Bluefruit.

### 7.6 Reductions

| Reduction | RAM | Flash | Verdict |
| --- | ---: | ---: | --- |
| Cache newest identity/backlog count (D2) | +8–12 B | ~0 | **Do** (CPU/latency, not bytes). |
| RX queue 255→64 B payload | −764 B | 0 | Defer until v2 max frame frozen. |
| Drop `geofence_boot_snapshot` (const-ref accessor) | −524 B | ~0 | Reject now: trivial bytes, source-contract churn. |
| Trim TinyUSB classes | ~−3 KB | ~−8 KB | LATER; framework config patch. |
| Merge gate staging with client buffers | ~−1 KB | 0 | **Do not**: weakens async source-stability invariant. |
| Remove geofence recovery workspaces | ~−2.8 KB | 0 | **Do not**: re-creates the 4 KiB stack risk. |

Projected headroom after all currently committed slices (status service,
geofence writer, secure envelope): RAM stays < 20 %, flash well below single-bank
ceiling. **RAM/flash are not the constraint; energy and field recovery are.**

---

## 8. Power report

All currents below are datasheet-class ESTIMATES. **No quantitative current has
been measured for this product** (M6P1 §7, M7P7B: DEFERRED).

| Subsystem | Active driver | Active time / wake frequency | Worst-case retry | Prevents sleep? | Failure > success? |
| --- | --- | --- | --- | --- | --- |
| GNSS (3V3_S rail, `SensorPowerManager`) | ZOE-M8Q acquisition ~25 mA, continuous ~20–25 mA, `powerSaveMode(false)` | Warm: 3–13 s per B (B > 60 s). Continuous when interval ≤ 60 s. | 120 s per interval, forever, no backoff | No (rail cut) | **Yes** (F-H5) |
| SX1262 TX | ~45 mA @ 14 dBm | 0.987 s per POSITION; relay 1.233 s per forward | 5 s TX timeout; no RF retry | No | No |
| SX1262 RX (tracker) | ~5 mA | 10 s after each TX, then `Radio.Sleep()` | — | No | n/a (F-M8) |
| SX1262 RX (BASE / relay / AUTO-BASE) | ~5 mA | Continuous by design | — | — | F-H2 makes it accidental on trackers |
| BLE | SoftDevice advertising (framework default interval) | 10 min after every boot/disconnect (applied to all roles today; no gateway-bridge profile exists) | 1 s restart retry | No | No |
| Accelerometer (LIS3DH) | µA-class | Probe then power-down; manual capture only | 3 attempts, 60 s fault-cleanup backoff | No | No |
| MCU loop | nRF52840 run ~3–6 mA | ~100 wakes/s (`delay(10)`) | — | **Yes** — limits idle depth | — |
| Flash | NVMC erase ~85 ms / program µs | 1 page erase per 104 records + per 8 boots | 4 s op / 6 s token timeouts | No | Storage fail → GNSS energy wasted (F-H3) |
| Watchdog | 30 s, runs in sleep | — | — | No | — |

Energy per useful position (ESTIMATE, B = 180 s, INSIDE, open sky):
GNSS ~75–325 + TX ~44 + RX window ~50 ≈ **170–420 mA·s**; average ≈ 1–2.3 mA
plus the unmeasured idle floor. OUTSIDE at 60 s continuous: ≈ 23 mA + radio ≈
**~25 mA average** (continuous-GNSS dominated). Obstructed: ≈ 17 mA average with
**zero** positions.

Answer to "can 60 s / B/3 create undesired continuous tracking?": **yes, with
the 180 s development default it always does while OUTSIDE**, by design of the
unchanged M3 short-interval rule. Whether that is *undesired* depends on the
measured crossover (§9). With a product-like B ≥ 15 min, B/3 ≥ 5 min and the
receiver is power-cycled. Do not change it in this audit.

---

## 9. GNSS report

State machine (`GnssManager::State`, 12 states + 6 flag sub-states) is robust
against the failure modes it was built for: stale PVT/DOP pairing (R3),
receiver backlog (R3.1/R3.2), I2C hangs (R4), no overlapping acquisitions,
M6D2 continuation within one session. Freshness is strict: 5 s from callback
to TX, re-checked under the radio gate. These are PHYS-validated behaviors.

Open GNSS issues are all **power-policy**, not correctness:

1. **No failure backoff** (F-H5). Repeated 120 s timeouts at every grid point.
2. **Timeout always power-cycles**, even in continuous mode (`needs_configuration_`
   blocks `keepTracking`). In an obstructed location a retained lock is worth
   more than a fresh cold/warm start; the field observation says a
   power-cycled indoor receiver can fail for hours.
3. **Binary regime switch at 60 s** (F-M10) with `powerSaveMode(false)`. The
   u-blox M8 power-save mode (cyclic tracking) and software backup
   (`RXM-PMREQ`, needs V_BCKP) are unexplored alternatives between "rail cut"
   and "full continuous". V_BCKP behavior is **not measured** (M6P1 §6).
4. **Detection is boot-only** (3 attempts), coupled to role (F-H2).
5. The 1 s rail settle + 5-step reconfiguration is part of every power-cycled
   TTFF (D3). It is needed today; its length was copied from the RAK example
   and never measured.

Recommended decision metric: **charge per accepted fresh position**, per
environment (open sky, canopy, barn/indoor), per policy (rail-cut, continuous,
PSM, backup). Measure with a PPK2-class instrument on the owned collar before
touching thresholds. Expected shape (to be confirmed): rail-cut wins in open
sky for B ≳ 2–5 min; continuous/PSM wins under obstruction; backoff must cap
the obstructed failure case regardless.

---

## 10. RF / airtime / scale report

PHY: 869.525 MHz, SF11, BW125, CR4/5, preamble 8, explicit header, CRC, LDRO
(auto). Airtime (MEASURED by formula, matches `ORUN_DOWNLINK_RENDEZVOUS_PLAN.md`):
POSITION 34 B = **987.136 ms**; RELAY_FORWARD 49 B = **1,232.896 ms**; TEST
18 B = 659.456 ms. (SF9 reference: 246.8 / 328.7 ms; SF7: 77.1 / 97.5 ms.)

Pure-ALOHA model, independent trackers, one relay that forwards everything it
hears (`relay=1`) or no relay (`relay=0`). `P_ok` = probability a given POSITION
does not overlap another frame at a receiver (capture effect ignored — real
results will be somewhat better). Relay duty = relay's own TX airtime share.

| N | B | relay | Load G | P_ok | Tracker duty | Relay duty |
| ---: | ---: | :---: | ---: | ---: | ---: | ---: |
| 10 | 180 s | 0 | 0.055 | 0.91 | 0.55 % | — |
| 10 | 180 s | 1 | 0.123 | 0.81 | 0.55 % | 6.9 % |
| 30 | 180 s | 1 | 0.370 | 0.51 | 0.55 % | **20.6 %** |
| 50 | 180 s | 1 | 0.617 | 0.32 | 0.55 % | **34.3 %** |
| 100 | 180 s | 1 | 1.233 | 0.10 | 0.55 % | **68.5 %** |
| 10 | 900 s | 1 | 0.025 | 0.96 | 0.11 % | 1.4 % |
| 30 | 900 s | 1 | 0.074 | 0.87 | 0.11 % | 4.1 % |
| 50 | 900 s | 1 | 0.123 | 0.80 | 0.11 % | 6.9 % |
| 100 | 900 s | 1 | 0.247 | 0.63 | 0.11 % | **13.7 %** |
| 100 | 1800 s | 1 | 0.123 | 0.79 | 0.06 % | 6.9 % |
| 1000 | 900 s | 0 | 1.097 | 0.11 | 0.11 % | — |

OUTSIDE cadence: at B/3 = 60 s each tracker costs the relay 2.05 % duty — the
10 % budget is reached by **~5 simultaneously OUTSIDE trackers**. At B/3 =
300 s, ~24.

Conclusions:

- **~10 devices:** safe at any cadence ≥ 180 s. Relay duplication is the main
  cost.
- **~30–50 devices:** safe only at product-like cadence (B ≥ 15 min) and with a
  bounded relay load. At the 180 s dev default it is unsafe (collision and duty
  cycle). An alarm burst (a herd crossing a fence ⇒ many B/3 trackers) is the
  realistic stress, not steady state.
- **~100 devices in one domain:** feasible only at B ≥ 30 min with limited relay
  overlap, or with a faster PHY (SF9 cuts airtime ~4×) chosen per link budget.
- **1000 devices:** impossible in one SF11 channel (G ≫ 1). This exposes no
  hidden trunk *code* defect — IDs are 64-bit, sequences 32/64-bit, dedupe keyed
  by (source, sequence) — but it confirms that PHY must stay configurable and
  that RF-domain partitioning is the scaling path, as the product architecture
  already states.

Other RF observations:

- No CAD/LBT; relay and trackers are pure ALOHA. Relay is deaf during its own
  1.23 s TX (half-duplex loss not modelled above).
- Relay queue 4 entries, dedupe 16; BASE dedupe 32 entries (RAM, cleared on
  reboot). At 50 trackers/180 s the BASE window covers ≈ 115 s — sufficient for
  relay copies (≤ 5.4 s later), insufficient for any future replay: the backend
  must own long-term dedupe.
- No downlink, no command/response storms, no configuration distribution, no
  EVENT bursts and no replay bursts exist yet — so none of those loads is
  currently possible. They must be budgeted in the v2 design (secure EVENT with
  ACK/controlled retry, replay pacing that never starves live traffic).
- TX power 14 dBm conducted; antenna ERP/regulatory validation remains an
  install-time obligation (CODE comment `radio_config.h`).

---

## 11. Storage / wear / recovery report

Partition map (CODE `storage_config.h`, enforced by `static_assert` chain and
`scripts/check_storage_layout.py`):

```text
0x026000 .. 0x0E5000  application (ceiling guard)
0x0E5000 .. 0x0E7000  GeofenceStore  A/B (2 pages)
0x0E7000 .. 0x0E9000  SecurityStore  A/B (2 pages)
0x0E9000 .. 0x0EB000  ConfigStore    A/B (2 pages)
0x0EB000 .. 0x0ED000  BLE bonds (relocated InternalFS)
0x0ED000 .. 0x0F4000  HistoryStore ring (7 pages)
0x0F4000 ..           bootloader/settings
```

Owner uniqueness: **PASS** (each region one backend; all mutations through
`FlashMutationGate`; InternalFS arbitrated by the M7P7A token). No overlap.

| Store | Commit model | Full/rollover | Unsupported-newer | Recovery | Physical evidence |
| --- | --- | --- | --- | --- | --- |
| History | per-record body+CRC then commit word; page header; seq & state slots | ring overwrite oldest (no priority) | v2 dev format ⇒ refuse (no erase) | CRC scan, monotonic identity check | **host only**; physical power removal pending |
| Config v2 | A/B, erase inactive, body+CRC stage, commit last, retire word | n/a | maintenance/reset required, never erased | UNCERTAIN + reconciliation | scoped PHYS |
| Geofence | A/B, 564 B record, commit last | n/a | never guessed as CLEAR | UNCERTAIN token keeps semantic snapshot | PHYS (M6D3B) |
| Security v2 | A/B + typed log, reserve-ahead counters | compaction | fail closed | burned-slot accounting | partial-erase sentinel PHYS |

Wear (ESTIMATE; nRF52840 datasheet endurance 10,000 cycles):

| Workload | History page erase period | Erases/page/year | Years to 10k |
| --- | ---: | ---: | ---: |
| B = 180 s | 36.4 h | ~241 | ~41 |
| 60 s continuous OUTSIDE | 12.1 h | ~724 | ~14 |
| B = 15 min | 7.6 d | ~48 | > 200 |
| + 1 reboot/day | +1 erase per 8 days of the ring | negligible | — |

Wear is not a risk. **Data loss by reboot (F-H1) is.** Config/Geofence wear is
bounded by their no-op-on-unchanged rule; future remote writers must also be
rate-limited because untrusted input must not drive erase cycles.

Power-cut coverage: the A/B stores have documented boundary-specific PHYS
evidence (no mid-NVMC/brown-out claim). HistoryStore's power-cut guarantees are
host fault-injection only.

---

## 12. Concurrency / ownership report

| Owner | Context | Mechanism | Verdict |
| --- | --- | --- | --- |
| `RadioManager` | loop; SX126x `LORA` task callbacks | FreeRTOS mutex driver gate (try-acquire from loop), `taskENTER_CRITICAL` handoff, TX generation, role epoch, `accept_rx_events_`, stale counters | **Sound.** Late TX results after role change are counted as stale; RX queue bounded; callbacks never touch application state. |
| `FlashMutationGate` | loop; Bluefruit SOC task forwards events | one in-flight slot, priority + aging, cross-task atomic token vs InternalFS, quarantine on accepted-but-timed-out ops, late-completion reconciliation | **Sound.** Known liveness gap: InternalFS holding the token on `portMAX_DELAY` (documented, physical BLE concern). |
| BLE application | loop; Bluefruit BLE task callbacks | `BleApplicationHandoff` mailbox under critical sections; session generation; ingress closed while response pending; terminal-error disconnect recovery | **Sound**; state lives in `main.cpp` globals (F-M1). |
| GNSS | loop only | SparkFun callbacks run inside `checkCallbacks()` on loop | Sound; one static driver instance. |
| Accelerometer / I2C | loop only | shared Wire bus serialized by cooperative loop; bounded R4 recovery | Sound. |
| Stores | loop only | cooperative `poll()`; TX guard (`!isTransmitting()`) | Sound; Geofence not polled (F-L1). |

Specific checks requested:

- *Callbacks after owner state changed:* handled by generation/epoch in radio,
  gate and BLE. PASS.
- *Duplicate completion:* gate ignores events after `event_ready`; counts spurious.
  PASS.
- *Lost wakeups:* disconnect counter is a monotonic counter compared with a
  loop-owned "seen" value; RX timeouts/errors are counted not flagged. PASS.
- *Ambiguous busy:* gate returns `kPending` both for "queued" and "submitted";
  callers never need to distinguish. PASS.
- *Timeout without reconciliation:* gate quarantines instead of releasing; Config/
  Geofence stores mark UNCERTAIN and reconcile. HistoryStore instead fail-stops
  (F-H3) — safe, not recoverable.
- *Lock ordering:* radio mutex → critical section only; flash token is a CAS,
  never held with the radio mutex. No inversion found.
- *Reset during pending work:* covered by store recovery semantics; HistoryStore
  per-boot reservation is where reset has a *cost* (F-H1).
- *Mutable global drivers:* `SFE_UBLOX_GNSS gnss`, `Radio`, `Bluefruit`, `Wire` —
  single-instance by construction; `active_*_manager` raw pointers assume one
  instance (true).

---

## 13. Security report

Current, factual state:

| Area | State |
| --- | --- |
| LoRa (TLP v1) | Plaintext, unauthenticated, no network identity. Anyone can read device ID + precise location, spoof POSITION for any ID, and drive relay airtime. Documented as development traffic. |
| BLE | `SECMODE_OPEN` application characteristics; GET_CONFIG readable by any phone in range during the admission window. Bonding exists (framework) but is not ORUN authorization (correctly). |
| USB | Unauthenticated `ROLE TRACKER/RELAY/BASE` (physical-access trust). |
| Secure envelope / anti-replay | SecurityStore reserve-ahead TX counters and A2D replay HWM persistence implemented; **no caller**; no provisioning; CC310 primitives test-only. |
| Command idempotency / CAS | 96-bit state tokens exist in Config/Geofence stores; no command path. |
| DFU | Bootloader signing/bank/rollback **UNKNOWN**; serial/UF2 only. |
| Private location exposure | Every POSITION broadcast is public. |

**Safe only because no protected writer exists yet:**

- ConfigStore `requestSave/requestReset` (tracking interval, battery capacity);
- GeofenceStore `requestReplace/requestClear` (permitted areas);
- HistoryStore `markDeliveredThrough/saveReplayCursor` (would allow a forged ACK
  to mark undelivered data delivered);
- SecurityStore `commitCredential` (credential install), A2D admission;
- role selection (only via USB, volatile).

Each of these must enter production only behind reviewed authentication,
authorization, anti-replay (durable, before side effects), idempotency/CAS and
authenticated RESULT — exactly as the existing ADRs require. Additionally,
before BLE carries location/geofence/health status, require an encrypted link
(LESC) even for read-only requests (F-M7). No custom cryptography is proposed.

---

## 14. Test architecture report

| Layer | What exists | Evidence value |
| --- | --- | --- |
| Unit / contract | codecs, geometry, operational SM, stores, gate, BLE framing, admission policy | high |
| Compatibility / golden | `tests/compatibility/test_legacy_packets.cpp` literal wire vectors + malformed rejection | high — **must stay frozen** |
| Fault injection | FaultFlash bit/byte/erase budgets (M4), async gate timeouts/late completion (M7P3/M7P5/M7P6/M7P7A), GNSS delayed pairs (R3), I2C (R4) | high |
| Sanitizers / warnings | every host binary `-Wall -Wextra -Werror -fsanitize=address,undefined` | high |
| Production startup harness | `tests/startup/test_startup.cpp` `#include`s `main.cpp` with stubs; 11 scenarios | medium — proves composition order, not hardware |
| Source-contract (Python) | M6D3B, M6D3C, M7P7G | mixed (below) |
| Build gates | storage-layout pre-script, pinned patch verification | high |
| Hardware probes | ~10 test-only envs (flash, crypto, geofence qual/power-cut, config power-cut) | high, scope-limited |
| Physical qualification | per-milestone records | scope-limited; see §3 |

Observations:

- **Source-contract tests:** keep the architectural assertions (forbidden
  dependencies of the provider, storage-before-SoftDevice ordering, no
  `requestReplace` in production). Some assertions pin text, e.g.
  `main.count("orun_tlp::geofence_format::Snapshot geofence_boot_snapshot") == 1`
  and log-string presence in M7P7G. These block harmless refactors and prove
  nothing behavioral; convert them to behavior tests when the code is touched.
- **Tests that claim more than they prove:** host power-cut tests prove the
  algorithm under a NOR model, not NVMC brown-out behavior (docs already say
  so). The startup harness uses Bluefruit/SX126x stubs; it cannot prove SoftDevice
  timing.
- **Missing, high value:**
  1. HistoryStore **reboot-loop property test** (would have caught F-H1):
     random sequences of append/reboot/cut; assert no sequence reuse *and* no
     record loss without new appends.
  2. Coverage-guided **fuzz** targets (libFuzzer, host) for
     `deserializePositionPacket`, `deserializeRelayForwardPacket`,
     `NetworkService::receive`, `BleApplicationTransport::onFrameReceived`,
     `config_format::inspectPagePrefix`, `geofence_format::inspectPage`,
     `security_format` decoders.
  3. GNSS **energy-accounting simulation** (policy × environment → charge per
     position) once a policy function exists.
  4. N-node **RF load simulation** (§10 model with relay/half-duplex/bursts),
     as `ORUN_PRODUCT_SYSTEM_ARCHITECTURE.md` §14.5 already requires.
  5. Storage fail-stop / recovery scenario in the startup harness (F-H3).
- **Physical cases not yet exercised:** HistoryStore power removal/brown-out;
  relay path since M6P1; M6D2 INSIDE→OUTSIDE→INSIDE confirmation; any
  quantitative current; SoftDevice-async geofence mutation; DFU preservation of
  0x0E5000–0x0F4000.
- **Duplicated tests with no extra evidence:** none found worth deleting.
  Recompilation overhead is a CI-time issue only.

---

## 15. BLE / mobile / backend completion gaps

What exists: typed `ApplicationRequestService` (one request kind, one global
response slot, requester provenance), frozen M7P7F framing (20 B frames,
48 B logical payload, 4 fragments, stop-and-wait indications), USB and BLE
converging on the same owner — the right trunk.

What is missing for bounded read-only status (the owner-prioritized M6D3C
follow-up):

| Status family | Owner that already has the truth | Gap |
| --- | --- | --- |
| Device | identity, firmware version, reset reason, role + AUTO/OVERRIDE | typed snapshot, uptime |
| Location | last accepted `GnssFix` (only transiently in `main.cpp`) | **no last-accepted-location owner**; must carry observation age/source/validity, never "live" after reboot |
| GNSS health | `GnssManager::Diagnostics`, `state()` | exposure |
| Geofence | `GeofenceStore` resource/token; coordinator INSIDE/OUTSIDE/unclassified, cadence mode, confirmation active | exposure; operational state not persisted (by design) |
| Power | **nothing** | battery measurement (F-H4); must report UNAVAILABLE until then |
| Radio | listen diagnostics, TX attempts/timeouts, RX/relay/base counters | exposure |
| Storage | History/Config/Geofence/Security diagnostics & readiness | exposure |
| Health | watchdog reset flag | stack high-water, fail-stop flags |

Each family must fit the 48 B logical payload or be split into separate request
kinds (enum + switch in the service — no framework). Requested vs effective
must be reported separately (e.g. durable B, applied B, effective B/3).

What backend/mobile can safely start now: backend domain model (devices,
positions with *observation time ≠ receipt time*, events, geofences), a
development ingestion adapter for BASE serial output, map rendering with
fresh/stale/unknown semantics, and the read-only BLE status screens once the
status contract is frozen. What must wait: commands, alarms/notifications
(need secure EVENT), delivery/ACK semantics, config/geofence distribution (CAS
wire unfrozen), LOST, backend dedupe that assumes sequence uniqueness across
history resets (F-M4).

---

## 16. Strategy Pattern assessment

Options compared for every candidate: **A** current direct code; **B** runtime
Strategy (interface + virtual); **C** compile-time policy/template; **D** plain
function/enum/switch. On Cortex-M4 a virtual call costs a vtable pointer per
object and ~3–5 extra cycles per call — bytes and cycles are irrelevant here; the
real costs are indirection, debugging and hidden hardware behavior.

| Candidate | >1 real strategy today? | Committed variation soon? | Removes duplication? | Testability | Verdict |
| --- | --- | --- | --- | --- | --- |
| Location source selection | No (GNSS only) | Phone location for MOBILE/SEARCH | No | D is enough | **DEFER** — when a 2nd source lands, a Location owner with an enum source + switch (D). |
| Tracking cadence | B and B/3 via pure `effectiveTrackingIntervalMs()` | movement/stationary, low-battery stages | No | already a pure function | **KEEP CURRENT** (extend inputs of the pure function). |
| Radio listen policy | Continuous / Windowed via pure `resolveRadioListenPolicy()` | none committed | No | already pure | **KEEP CURRENT**. |
| Forwarding policy | One-hop v1 only | v2 undefined | No | — | **DO NOT INTRODUCE**. |
| GNSS power policy | Implicit rail-cut vs continuous inside `enterLowPower()` | backoff, PSM/backup candidates, low-battery | Yes — decision logic entangled with SM | a pure decision function would allow policy×environment simulation | **SMALL POLICY EXTRACTION** (D, pure function; no virtual). |
| Retry/backoff | several unrelated, each correct for its semantics | GNSS backoff | No | — | **DO NOT INTRODUCE** a generic retry framework. |
| Geofence cadence | `effectiveTrackingIntervalMs()` | — | — | — | **KEEP CURRENT**. |
| Transport adapters (USB/BLE/LoRa) | yes, but framing genuinely differs; domain already shared via `ApplicationRequestService` | BLE status kinds, later LoRa v2 | No | — | **KEEP CURRENT** (typed request enum + switch). |
| Role/profile behavior | `legacyRoleBehavior()` table | profiles as data | Classes would *add* coupling | data is more testable | **DO NOT INTRODUCE** role strategy classes; make profiles persisted *data* (F-H2). |
| Flash backend | already `FlashBackend` interface (B) | — | enables host fault injection | yes | **KEEP** (the one place B already pays for itself). |

Strategy Pattern does not reduce airtime or GNSS current. The only
behavior/resource improvement available here comes from the GNSS power-policy
extraction, and it comes from making the *decision* testable, not from dispatch.

---

## 17. Other design-pattern opportunities (only where justified)

1. **Extract two loop-owned components out of `main.cpp`** (plain classes, no
   interfaces): `GeofenceTrackingComposer` (episode buffers, slot-0 guard,
   representative queue, cadence application, abort rules) and
   `BleApplicationRuntime` (session, indication, terminal-error disconnect).
   Justification: F-M1 — unit-testable behavior instead of text contracts,
   before the geofence writer and LOST slices add more state. Zero behavior
   change; same call order in `loop()`.
2. **Typed status snapshot structs** per family (§15) owned by their existing
   subsystems, assembled by `ApplicationRequestService`. Not a pattern — just a
   read model that prevents USB/BLE from each formatting domain state.
3. Nothing else. No Observer/event bus (loop order is the contract), no
   Repository (stores are already the owners), no Factory, no HAL.

---

## 18. Dead / deferred / speculative code report

| Item | Location | Classification |
| --- | --- | --- |
| M1 TEST beacon TX path (`kTestBeaconEnabled=false`, `sendTestPacket`, jitter, `next_tx_at_ms_`) | `radio_manager.cpp`, `radio_config.h` | KEEP FOR COMPATIBILITY until v2 cutover, then REMOVE |
| TEST RX decode/log | `radio_manager.cpp:668-683` | KEEP FOR COMPATIBILITY |
| `RadioManager::encodePosition` "compatibility shim for host seams" | `radio_manager.h/.cpp` | KEEP TEST-ONLY → REMOVE when tests move to `encodeLegacyPosition` |
| RAK identity fallback inside `RadioManager::begin` | `radio_manager.cpp:89` | KEEP TEST-ONLY (host seams) |
| History delivered/replay APIs | `history_store.h` | KEEP DEFERRED (store-forward) |
| Config/Geofence/Security mutation APIs | stores | KEEP DEFERRED (0 B linked) |
| Config v1 legacy codec | `config_format` | KEEP FOR COMPATIBILITY (maintenance detection only) |
| SecurityStore v1 read/migration | `security_store.cpp` | NEEDS DECISION (no deployed fleet) |
| Activity window/quality/capture | `activity_*` | KEEP DEFERRED (diagnostic seam for M6B) |
| `#ifdef ORUN_M7P6E_CRYPTO_BLE_PROBE` (~290 lines) | `main.cpp` | KEEP TEST-ONLY; NEEDS DECISION to retire (evidence closed) |
| `#ifdef ORUN_M7P7B_FLASH_PROBE` (~175 lines) + `include/m7p7b_flash_probe.h` | `main.cpp`, `include/` | KEEP TEST-ONLY; NEEDS DECISION to retire; header belongs under `tests/` |
| `#ifdef ORUN_M6D2_GEOFENCE_PROBE` (~95 lines) | `main.cpp` | KEEP TEST-ONLY until M6D2 physical qualification closes |
| `AccelerometerDiagnosticState` mirror | `main.cpp:585-595` | KEEP (trivial) |
| `kFuture*` names for owned regions | `storage_config.h` | KEEP (documented churn avoidance) |
| Unused board/variant files rak11200/rak11300/rak3112 | `rakwireless/` | NEEDS DECISION (REMOVE candidate; not speculative ORUN code, just unused vendor files) |
| Stale README §3 | `docs/architecture/README.md` | NEEDS DOC FIX |

No speculative HAL or second-platform driver exists — the discipline in
AGENTS.md is being followed.

---

## 19. Optimization candidates (behavior-preserving unless stated)

**O1 — Cache newest identity and backlog count in `HistoryStore`.**
Current: full decode scan in `append()` and per log line. Proposed: maintain
`newest_identity_` and `backlog_count_` from `recover()`/`finishBlob()`/erase.
RAM +8–12 B · Flash ≈ 0 · CPU −2 scans/fix (ESTIMATE 20–60 ms) · Energy small
negative · RF none · Risk low (recovery must initialize both) · Tests: M4 suite +
new equality property vs full scan · Physical retest: NO (host sufficient; build).

**O2 — Stop per-boot history page erasure (F-H1).** Two candidate designs,
both need independent review: (a) *lazy reservation* — reserve only when the
first ticket of a boot is requested (removes cost for boots with no fix); (b)
derive the next ticket from the newest committed record, since store-first
guarantees every transmitted sequence is durable (reservation then only needed
if the TEST beacon, which transmits without storing, stays enabled). Wire
bytes unchanged; storage format unchanged for (a). RAM 0 · Flash ~0 · Wear −1
erase/8 boots · Risk medium (no-reuse invariant) · Tests: reboot-loop property,
M4 cut matrix · Physical retest: **YES** (reboot/power-cut sentinel).

**O3 — GNSS acquisition-failure backoff** (behavior change, owner decision).
e.g. after K consecutive timeouts, stretch attempts to k·B up to a cap, reset
on success or on INSIDE→OUTSIDE evidence. Energy: bounds obstructed mode from
~17 mA to a chosen budget (ESTIMATE) · RF none · Tests: policy unit tests +
M3/R3 regression · Physical: **YES**.

**O4 — Do not cut the rail on timeout while in continuous mode** (owner
decision). Keeps lock/almanac when obstructed. Needs measurement first ·
Physical: **YES**.

**O5 — Production stack/heap watermark in diagnostics.** `uxTaskGetStackHighWaterMark`
for loop/LORA/BLE tasks + `xPortGetFreeHeapSize`-equivalent. RAM ~0 · Flash
< 0.5 KB · no behavior change · Physical: YES (read the numbers once).

**O6 — Loop idle period.** Measure first; if the 100 Hz floor is material, drive
wakes from actual deadlines (GNSS 100 ms while acquiring, radio IRQ, BLE
events) instead of fixed 10 ms. Risk medium (latency assumptions in R2/R4) ·
Physical: **YES**.

**O7 — Tracker RX window length** (owner decision, F-M8): keep 10 s only when a
downlink consumer exists; until then shorter or zero. Energy −~50 mA·s/TX
(ESTIMATE) · RF: none today · Physical: **YES**.

**O8 — TinyUSB class trim** (LATER): ~−8 KB flash / ~−3 KB RAM, framework patch
cost. Reject for now: resources are not the constraint.

**O9 — Table-driven CRC32**: +1 KB flash, ~5× faster CRC. Reject: O1 removes the
hot path; remaining CRC use is not material.

Rejected as "trivial bytes, added complexity": boot snapshot removal,
staging-buffer sharing, RX queue shrink before v2, merging sibling flash
backends.

---

## 20. Recommended roadmap

Each item lists: root cause · component · product effect · compatibility · RF ·
power · RAM/Flash · storage/wear · security · tests · physical validation.

### NOW — high value, low risk, before adding features

**NOW-1 Stop reboot-driven history loss (F-H1, O2).**
Root: per-boot reservation erasing data pages · `HistoryStore::begin/startReservation` ·
history survives brown-out/watchdog loops · TLP v1 bytes unchanged; journal format unchanged for lazy reservation ·
RF none · power none · RAM/Flash ≈ 0 · wear reduced · security: no-sequence-reuse must hold ·
tests: reboot-loop property test, M4 cut matrix · physical **YES**.

**NOW-2 HistoryStore hot-path caching (F-M3, O1).**
Root: O(N) scans · `HistoryStore::append/backlogCount`, `main.cpp:2001` ·
lower loop latency · none · none · tiny energy gain · +12 B RAM · none · none ·
tests: equality vs full scan · physical NO.

**NOW-3 Quantitative energy campaign (no code).**
Root: every energy decision is ESTIMATE · PPK2-class measurement on the owned
collar: idle floor (100 Hz loop), GNSS warm/continuous/timeout per environment,
TX, 10 s RX window, BLE window · unlocks F-H5/F-M8/F-M9/F-M10 decisions ·
none · none · — · — · — · — · record in a milestone doc · physical **YES** (it *is* the physical work).

**NOW-4 Stack/heap watermark diagnostics (F-M6, O5).**
Root: invisible margin · health counters · serviceability · none · none · none ·
< 0.5 KB flash · none · none · tests: startup harness · physical YES (read once).

**NOW-5 Test gaps.** Reboot-loop property (NOW-1), fuzz targets for all decoders,
storage fail-stop startup scenario. No production change · physical NO.

**NOW-6 Documentation hygiene.** Fix README §3 (F-L4); record in
`ORUN_FIELD_NETWORK_DIAGNOSTICS_PLAN.md` or a new note the §10 airtime/duty
table and the reboot-loss property; mark relay path "not re-run since M6P1"
in the completion table. Physical NO.

### NEXT — required product completion work (ordered by dependency)

```text
NOW-3 energy data ───────────────┐
                                 ├─> NEXT-4 GNSS power policy
NEXT-2 battery measurement ──────┘        │
        │                                 │
        ├─> NEXT-3 storage fault recovery │
        │                                 │
NEXT-1 persisted profile/role intent      │
        │                                 │
NEXT-5 extract composers from main.cpp ───┼─> NEXT-6 read-only status (USB → encrypted BLE)
                                          │           │
                         NEXT-7 TLP v2 trunk contract (network id, epoch, secure EVENT, ACK, airtime)
                                          │
                         NEXT-8 secure envelope + provisioning ─> NEXT-9 store-forward replay/ACK
                                          │                        │
                                          └─> NEXT-10 geofence writer (poll!) ─> NEXT-11 LOST
```

**NEXT-1 Persisted profile/role intent (F-H2).**
Root: role inferred from GNSS presence, volatile override · `RoleController`,
ConfigStore schema (explicit requested services / relay enable / profile) ·
tracker never silently becomes BASE; relay survives reset · ConfigStore v2
layout change → needs its own reviewed format slice; TLP v1 unchanged ·
RF: removes accidental continuous RX · power: large positive on failure path ·
RAM/Flash small · wear: one write per change · security: local USB write now,
protected remote later · tests: B3/B4 + startup scenarios for GNSS-absent tracker ·
physical **YES**. Also add bounded periodic GNSS re-detection when tracking is
requested.

**NEXT-2 Battery measurement + staged low-battery policy (F-H4).**
Root: no VBAT path · new small owner + variant pin (confirm RAK19007 divider from
schematic, not assumed) · battery state visible; brown-out loops mitigated ·
none · reduces TX/GNSS at low stages · small · none · none · tests: policy unit +
ADC stub · physical **YES**.

**NEXT-3 Bounded storage-fault recovery (F-H3).**
Root: permanent fail-stop · `HistoryStore::fail`, `PositionFlow` · device recovers
without service; health shows fault · none · none · stops wasted GNSS energy while
storage is down · small · recovery must be read-only re-scan before resuming ·
none · tests: fault-injection scenario · physical YES.

**NEXT-4 GNSS power policy (F-H5, F-M10, O3, O4).**
Root: implicit policy, no backoff · pure `gnssPowerDecision()` extracted from
`enterLowPower()` + backoff state · energy per position bounded in obstructed
mode · none · none · large (measured) · tiny · none · none · tests: M3/R3/M6D2 +simulation · physical **YES**.

**NEXT-5 Extract `GeofenceTrackingComposer` and `BleApplicationRuntime` (F-M1).**
Zero behavior change; unit tests replace text contracts · physical: focused
regression (M6D3C cadence, M7P7G GET_CONFIG).

**NEXT-6 Read-only Device Service (M6D3C follow-up, F-M11, F-M7).**
Typed status families (§15) through `ApplicationRequestService`; USB first; BLE
only on an encrypted link; location with observation age, never "live" after
reboot · new BLE request kinds (M7P7F framing unchanged) · physical **YES**
(Android).

**NEXT-7 TLP v2 trunk contract (F-M4, F-M5).** Network/domain identity,
sequence epoch/incarnation, secure EVENT with ACK/controlled retry, delivered
cursor semantics, per-device airtime budget, regional policy as configuration.
Documentation + review first; no code until frozen.

**NEXT-8 Secure envelope + provisioning** per existing ADRs.

**NEXT-9 Store-forward replay + ACK** (needs NEXT-7/8). Live-first, paced replay,
critical-event priority; consider history capacity/record-class review (F-M2).

**NEXT-10 Geofence protected writer** — must add `geofence_store.poll()` (F-L1),
rate limits, CAS/idempotency, authenticated RESULT.

**NEXT-11 LOST** after authenticated contact semantics.

### LATER — useful, legitimately deferrable

- RF load simulator (§10 model + bursts + half-duplex) before buying 30–50 units.
- History capacity/compaction and activity summary records.
- Relay forwarding suppression policy (v2; RSSI/overheard-ACK based).
- PHY/data-rate selection per link budget; RF-domain partitioning.
- TinyUSB trim (O8), RX queue shrink after v2 max frame.
- DFU/bootloader authenticity; decide single- vs dual-bank (§7.1 headroom).
- Retire closed test probes from `main.cpp`; move probe header to `tests/`.
- Host-test parallelization.

### DO NOT DO (no justified payoff now)

- Generic Strategy/Factory/Observer/Repository frameworks; role strategy classes.
- A generic flash HAL or merging the four A/B stores into one "generic store".
- Removing FlashMutationGate staging buffers or geofence recovery workspaces.
- Rewriting `GnssManager`'s state machine (extract the *decision*, keep the SM).
- An event-driven scheduler replacing `delay(10)` before NOW-3 measurements.
- Dynamic allocation or STL containers in firmware.
- Multi-hop/mesh, LoRaWAN, multi-channel/concentrator support.
- Changing the 60 s threshold, B/3, or the 10 s RX window before measurement.
- Shrinking RAM for its own sake (boot snapshot, RX queue, TinyUSB) at this stage.
- Deleting architectural source-contract assertions or any golden fixture.

---

## A. Merge / architecture health verdict

**HEALTHY WITH TARGETED FIXES.**

No finding requires architectural correction before the next feature. The trunk
(ownership, persistence, radio concurrency, compatibility discipline) is sound.
The targeted fixes are field-recovery and energy items that are cheap relative
to their product value, plus one structural extraction (NEXT-5) that should
precede the next stateful feature landing in `main.cpp`.

## B. Top 10 actions (product value × risk reduction)

1. Stop reboot-driven history erasure (NOW-1).
2. Persist explicit profile/role intent; GNSS absence must not make a tracker a BASE (NEXT-1).
3. Battery measurement + staged low-battery policy (NEXT-2).
4. Quantitative energy campaign on the owned collar (NOW-3).
5. GNSS failure backoff and continuous-mode timeout behavior, decided from data (NEXT-4).
6. Bounded recovery from HistoryStore fail-stop (NEXT-3).
7. Read-only Device Service over USB, then encrypted BLE (NEXT-6), preceded by the `main.cpp` extractions (NEXT-5).
8. TLP v2 trunk contract with network id, sequence epoch, secure EVENT/ACK and airtime budget (NEXT-7).
9. HistoryStore hot-path caching + stack/heap watermarks (NOW-2, NOW-4).
10. Missing tests: reboot-loop property, decoder fuzzing, storage-fault scenario (NOW-5).

## C. Things that look ugly but should deliberately NOT be refactored yet

- Four sibling `Nrf*Flash` backends and four near-identical gate client paths.
- `FlashMutationGate` staging buffers duplicating client blobs.
- Multiple geofence geometry copies and 4 KB `GeofenceStore` workspace.
- The dense single-line style of `history_store.cpp` / `journal_format.cpp`
  (proven code; rewrite only together with NOW-1/NOW-2 changes).
- Repeated POSITION (de)serialization/validation on the store and TX path.
- `legacyRoleBehavior()` projection and `NodeRole` — replace by persisted
  profile data (NEXT-1), not by a class hierarchy.
- The M1 TEST beacon path and v1 codecs — remove only at the v2 cutover.
- `kFuture*` storage names.
- `GnssManager`'s 12-state + flag machine.
- The relay's duplicate forwarding of packets the BASE also hears (it is the
  current reliability mechanism).
- `delay(10)` idle — until measured.
- Source-contract tests that encode architecture boundaries.

---

## Appendix — audit reproduction

```bash
git fetch origin && git worktree add --detach <dir> b4d97d9a02923e68aaf4806c344a129825fce4e3
cd <dir>/firmware && pio run -e rak4630                      # 28,744 B RAM / 260,008 B flash
PLATFORMIO_BUILD_FLAGS=-fstack-usage PLATFORMIO_BUILD_DIR=<tmp> pio run -e rak4630
cd <dir> && bash firmware/tests/run_host_tests.sh            # EXIT 0
arm-none-eabi-nm -S --size-sort -C .pio/build/rak4630/firmware.elf
```

The F-H1 host probe compiles the unchanged `history_store.cpp`,
`journal_format.cpp`, `tlp_position_packet.cpp` and
`legacy_position_mapping.cpp` against a synchronous NOR model, appends 600
records, then performs 64 clean `HistoryStore::begin()` reboots without
appends, printing `count()` and erase totals. It is not part of the repository.