# ORUN — Feature, Implementation, Evidence and Deferral Inventory

**Audit checkpoint:** 2026-10-09, canonical `main@336ab8f13f1f2ced4eb18034e4a05ac131e0f4e1` (after SF5D3 / PR #88).
**Status:** product documentation and source-evidence inventory, **not a code change,
new approval, fresh physical test, or exhaustive line-by-line security audit**.
All 490 entries of the `main` recursive Git tree were indexed; relevant
production `firmware/include`, `firmware/src`, the host-test runner,
selected test implementations, canonical architecture docs and PR/milestone
history were compared. The last owner-provided actual board evidence was
RAK-1 normal production upload + GNSS FIX / POSITION TX / HISTORY append on
2026-10-09. No new test has been run by this inventory writer.
**Evidence must be attributed to its actual code head and scope.**

This is the **navigation/status companion**, not a replacement for:
[`AGENTS.md`](../../AGENTS.md),
[`ORUN_PRODUCT_SYSTEM_ARCHITECTURE.md`](ORUN_PRODUCT_SYSTEM_ARCHITECTURE.md),
[`ORUN_CURRENT_ARCHITECTURE_RULES.md`](ORUN_CURRENT_ARCHITECTURE_RULES.md),
[`SF5.md`](../milestones/SF5.md), or each milestone's frozen contracts.
Older 2026-09-30 [system health audit](../audits/ORUN_SYSTEM_HEALTH_AUDIT_2026-09-30.md)
is historical; its pre-October missing-feature conclusions must **not**
override newer code/PRs.

## 1. The naming decision: no more exclusive product-device types

| Concept | One consistent product-facing name | What it is | Existing implementation / migration rule |
|---|---|---|---|
| Physical object | **ORUN cihazı / ORUN düğümü** (`ORUN node`) | Stable device ID, board, radio and installed modules | Device identity already implemented; **do not rename persisted IDs** |
| Hardware | **Donanım / yetenek** | GNSS, RAK1904 accelerometer, other sensors, GPIO/actuator driver, USB/BLE, power | `CapabilitySnapshot` currently covers **GNSS and accelerometer only**; presence does not mean service enabled |
| Function | **Servis / görev** | Konum takibi, sensör ölçümü, aktivite, Relay, Gateway köprüsü, vana/aktüatör kontrolü | Each has independently requested/effective state *as it is implemented*, with reasons; B4 currently handles Tracking + Relay, **not all listed services** |
| User preset | **Profil** | Hayvan, Sensör, Sulama, Ağ İstasyonu, **Gateway** (tek varsayılan profil örneği) | These are editable initial defaults, **not permanent node categories** and not yet a persisted profile editor. No `Sabit Gateway` / `Gezer Gateway` user-visible profile split. |
| Where/how installed | **Kurulum / kullanım bağlamı** | Hayvana takılı, direğe sabit, araçta/gezer; USB/telefon/Edge bağlı | A single **Gateway** function applies to each; `sabit/gezer` describes installation/mobility, **not a different product name, service or protocol**. |
| Upstream service | **Gateway Köprüsü** | LoRa side to a *connected* phone/Edge/host | **Not identical** to a bare legacy BASE receiver, and does not imply IP on RAK4631 |
| Reliable foreign hold | **Gateway Kalıcı Teslim Sorumluluğu** (`durable custody`) | Explicit authenticated responsibility for exact foreign object | Not the same as Relay; ACK requires review and durable proof |
| Legacy firmware compatibility | `NodeRole::kTracker/kRelay/kBase`, `ROLE TRACKER/RELAY/BASE` | Existing RF/boot-time compatibility values **for developers** | Keep exact old C++ enum, USB commands, wire bytes and test fixtures temporarily; **never expose them as additional product device types or user-facing choices**. Do not call legacy BASE a working Gateway. Plan explicit compatibility retirement after tested configuration migration. |

**Do not say:** "five different ORUN firmwares", "Sabit Gateway/Gezer Gateway are separate names", "this device is permanently a
Relay", "any sensor implies automatic Relay", "Gateway ACK means backend
delivered", "a Valve controller cannot relay", or "128 KiB for each service".

**Permitted composability — an existing architecture decision, NOT invented
by this inventory:** One node may have GNSS + accelerometer; only fixed
environmental sensors; environmental sensor + local valve driver + optional
Relay; fixed Relay + its own sensor reports; or a **Gateway** mounted on a fixed site or vehicle that
also originates data and/or relays. Vana+Relay is a **planned permitted**
combination, **not** a claim that production valve control is coded.

**Power default:** animal-collar profile keeps Relay **OFF**. Relay-enabled
service requires continuous SX1262 RX *between* half-duplex local
transmissions, with energy/airtime/regulatory trade-offs. No hardwired
mutual exclusion of sensors, tracking, actuation, forwarding or bridging is
acceptable in the **target** model. Existing legacy profile behavior is
intentionally still more restricted.

## 1A. What the operator actually sees — **one Gateway** and no legacy-role maze

**Only one user-visible gateway label: `Gateway`.** A Gateway can be
mounted, solar-powered, vehicle-carried or hand-carried. That difference is
deployment metadata, for example `Kurulum: Sabit` or `Kurulum: Gezer`
where genuinely useful, **never** a separate selectable service, firmware,
protocol, SKU, permanent `Sabit Gateway` / `Gezer Gateway` title,
permission level or custody guarantee. It needs an appropriate connected
phone/host/Edge for its data-bridge work.

Target configuration surfaces should display **one ORUN device identity** and
its actual services, e.g.:

```text
ORUN • Sulama İstasyonu                    (example, not a deployed UI)
Kurulum: Sabit
Donanım: Toprak nem sensörü, vana sürücüsü, LoRa
Konum Takibi: Kapalı
Sensör Raporlama: Açık
Vana Kontrolü: Açık (yalnız yetkilendirildiğinde)
Relay: Açık
Gateway: Kapalı
```

```text
ORUN • Araç İstasyonu                     (example, not a deployed UI)
Kurulum: Gezer
Konum Takibi: İsteğe bağlı
Sensör Raporlama: İsteğe bağlı
Relay: Açık
Gateway: Açık (bağlantı varsa)
```

The **future user interface must not offer `TRACKER`, `RELAY` and
`BASE` as mutually exclusive device choices**. Services must not be
silently replaced by legacy roles, even if old internals still contain
those strings. A Relay service can be enabled alongside valve/sensor/tracking;
a Gateway service may also be enabled independently when its actual host
transport/custody prerequisites are satisfied.

**Compatibility isolation (non-destructive, in stages):**
1. **Now — documentation / no runtime change:** show product-facing
   `Konum Takibi / Sensör / Vana / Relay / Gateway` concepts in user-facing
   plans. Restrict `NodeRole`/old `ROLE?` labels to firmware developer
   diagnostics, migration records and v1 tests; when they must be mentioned,
   prefix `Eski firmware modu` with current effect. Never translate
   `BASE` to `Gateway` as if the old receiver has an Edge bridge.
2. **Later — after persistence contract review:** save a node's independent
   requested services and observable effective states; safely migrate known
   existing settings without erasing ConfigStore, overriding provisioned
   settings or inferring service from GNSS presence. Make the app/editor and
   status surfaces use **services only**. Retain a compatibility adapter while
   the old firmware and golden TLP v1 fixtures remain supported.
3. **Only after tests & review:** retire redundant runtime role selection and
   developer alias surfaces once the replacement is physically validated.
   Prove upgrade/reboot/DFU, mixed old/new nodes, RF fixtures, reconfiguration
   and rollback first. **Never global-search/replace `BASE`→`Gateway`**
   or renumber wire codes, change packet bytes or reinterpret stored state.

This prevents **two competing sources of truth**: a future saved requested
service configuration is authoritative; legacy auto-role projection is an
explicit, bounded pre-migration fallback only. Existing normal-production
`ROLE?` remains available as a developer tool until the migration is
completed, not as a permanent separate product taxonomy.

## 2. Evidence/status vocabulary — do not inflate a PASS

- **P — Production wired:** code is included and called from the normal
  `rak4630` firmware; this alone does not prove a field scenario.
- **H — Host verified:** source plus automated model/fixture coverage,
  possibly sanitizer/warnings gates; neither RF nor brown-out proof.
- **F — Physical scoped:** a specified observation/test passed on a real
  RAK board. It is NOT proof of every deployment, energy load, channel, DFU or
  outage scenario; note the code head and scenario.
- **A — Architecture or transport foundation:** code contract or
  reusable component exists, but end-to-end production service does not.
- **D — Design/deferral:** chosen direction documented but implementation
  deliberately postponed or gated.
- **N — No implementation found in indexed `main` tree:** absence is
  bounded to this SHA; not a product prohibition.

A feature can have multiple labels: e.g. `P+H, physical pending`.
"Complete" always means **complete for its named narrow milestone**, not
market-ready product completion.

## 3. Full feature matrix at this source checkpoint

### 3A. Identity, role, configuration, hardware

| Feature | Actual status / evidence | Already reusable infrastructure | Missing / next gate |
|---|---|---|---|
| RAK4631 nRF52840 + SX1262 firmware build | **P+H+F**; 2026-10-09 `rak4630` `SUCCESS`, production upload 34.04 s, RAM 29,536 B / 248,832 (11.9 %), Flash 285,924 B / 815,104 (35.1 %) | `firmware/platformio.ini`, pinned framework/driver patches | Other MCU/board ports are **not** implemented; reference platform remains RAK |
| Device identity | **P+H; historical physical direct RF** | `rak_device_identity.cpp`, `device_identity.h`, B1/B4 tests | Provisioned application ownership/group identity still separate |
| One universal codebase / separable concepts | **Approved architecture**, existing from B4 / PR #42, **not new** | `AGENTS.md`, current rules, product architecture | Actual user-facing independent profile selection not wired |
| Legacy `TRACKER / RELAY / BASE` and AUTO boot decision | **P+H** (compatible, not desired final UX) | `node_role.cpp`, `node_behavior.h`, `main.cpp`; `ROLE?` and USB override | AUTO currently derives legacy role from GNSS presence; manual override **volatile**. Replace with persisted explicit service intent, retaining a compatibility transition |
| Separate Tracking/Relay effective resolver | **H and partly P**; B4 test asserts **both enabled** in a pure config; `RadioManager` can change forwarding state independently | `runtime_config.h/.cpp`, `test_b4.cpp`, `test_b4_network.cpp`, `radio_manager_relay_config.cpp` | Production `main.cpp::resolveRuntimeConfig()` still obtains requested flags from mutually exclusive legacy role; **no persisted combined-service switch or physical combined test** |
| GNSS/accelerometer discovery and health | **P+H; targeted physical M6A and GNSS evidence** | `CapabilitySnapshot`, `AccelerometerManager`, `GnssManager` | Generic arbitrary environmental sensor capability registry/driver registry not implemented |
| ConfigStore v2 → v4 | v2: **P+H+scoped F**; A/B tokenized safe persistence. v4 (2026-10-10, `feat/config-store-v4`): **H+build+partial F** — adds `service_mode` + requested services; reads v2/v4, writes v4. On one receiver: v2→v4 upgrade and v4→old→v4 downgrade passed; blank baseline, power cuts and BLE-connected save pending | `config_store.cpp`, `config_format.cpp`, `FlashMutationGate`, PR #46/#49; `ORUN_CONFIG_STORE_V4_SERVICE_INTENT.md` | Saved service intent is **not yet used at boot** and has **no writer** (next slice). No profile, Gateway, sensor, valve or location-source intent. No authenticated app writer |
| Device/status querying | **P+H+scoped F for M7P7H**; USB+BLE GET_CONFIG, DEVICE, TRACKING/GNSS, GEOFENCE, STORAGE | `application_request.cpp`, `application_status_runtime.cpp`, BLE/USB adapters | No measured battery status, comprehensive RADIO/HEALTH app family, GET_LOCATION product operation, secure mutations or generic dashboard |
| Location source-neutral accepted value | **P+H**, GNSS producer only (M7P7I / PR #63) | `location_owner.h`, `main.cpp` | PHONE/MANUAL/fixed sources and source-switch permission/ownership, GET_LOCATION, app map all **deferred** |

### 3B. GNSS, activity, environmental sensors, geofence and alerts

| Feature | Actual status / evidence | Already reusable infrastructure | Missing / next gate |
|---|---|---|---|
| GNSS acquisition + fix validation/freshness | **P+H+F**; owner 2026-10-09: first 109.021 s, second 2.974 s; 4 satellites, TX positions 27138/27139 | `gnss_manager.cpp`, `gnss_utc.cpp`, R3 freshness, R4 Wire recovery | Cold/warm TTFF variability, obstructed-view backoff, and current draw unresolved; no 100% fix guarantee |
| Periodic tracking and configured interval | **P+H+F** for GNSS/legacy POSITION | `gnss_config.h` default B=180 s, ConfigStore durable interval, M6D cadence | Rich configurable reporting/sampling policy and event-inclusive SF5 product records not live |
| RAK1904/LIS3DH accelerometer | **P+H+scoped F** M6A: probe, safe setup, sample, power-down/recovery | `accelerometer_manager.cpp`, `accelerometer_sample.h`, test M6 | No broad accelerometer hardware abstraction or measured continuous-energy envelope |
| Activity window diagnostics | **P+H+scoped F** M6B3: `ACTIVITY START`, `ACTIVITY?`, bounded window/quality diagnostics | `activity_capture.cpp`, `activity_window.cpp`, `activity_quality.cpp` | No routine classifier that identifies grazing/walking/resting on air; no period-level stored activity summary in production |
| Environmental sensors (temperature/humidity/soil/moisture/pressure/water) | **Architecture D; N for drivers and product reporting in current tree** | Generic sensor/service ownership guidance, RAK reference I2C, bus recovery, typed observation direction | Each actual sensor driver, identifier, calibration, unit/schema, sampling plan, storage and RF reporting is a separate future slice |
| Other digital/analog inputs and GPIO assignment | **D/N** | Capability/presence-vs-config distinction; `SensorPowerManager` controls a shared sensor rail | No generic analog/dry-contact channel assignments, isolation contracts or plug-in sensor registry |
| Geofence geometry and multiple areas | **P+H**, selected physical geofence-related tests | `geofence_geometry.cpp`, `geofence_area_set.cpp`, 8 areas / max 64 total effective vertices, M6C/M6D | Drawing areas and sending configuration from app/backend not live |
| Local geofence INSIDE/OUTSIDE, confirmation and adaptive B/3 | **P+H**, scoped physical runtime/boot evidence | `geofence_confirmation_coordinator.cpp`, `geofence_runtime_policy.cpp`, `geofence_runtime_provider.cpp`; M6D2/M6D3C | **No configured production fence from approved user writer**; full configured field transition and trusted RF alarm not complete |
| Durable geofence configuration storage | **P+H+scoped F**, read/recovery from two A/B pages | `geofence_store.cpp`, Geofence token ownership, M6D3B/C | BLE/LoRa authorized writer, app editor and distribution service missing |
| Critical OUTSIDE EVENT and remote alert delivery | **D/A** event semantic/observation format exists | M6D local OUTSIDE occurrence; SF5A/B EVENT and durable-store design | Secure PRODUCT_SECURE EVENT runtime, acknowledgments, backend/app push notification missing |
| LOST, autonomous search, FREE_GRAZE | **D**, local prerequisites partly implemented | Local location, geofence and timing, planned contact/security rules | Authenticated contact evidence and LOST state machine; MOBILE SEARCH field workflow; FREE_GRAZE classification unimplemented. `NEAR_FENCE` explicitly deferred |

### 3C. LoRa, Relay, Gateway and RF scale

| Feature | Actual status / evidence | Already reusable infrastructure | Missing / next gate |
|---|---|---|---|
| Private SX1262 LoRa P2P RX/TX | **P+H+F**; Tracker POSITION TX and Base reception, real hardware | `radio_manager.cpp`, `radio_driver_gate.cpp`, fixed `radio_config.h` | RF setting persistence, terrain/range and quantitative airtime/load validation |
| Frozen TLP v1 POSITION + TEST codecs | **P+H+F** POSITION; TEST TX disabled by default | `tlp_position_packet.cpp`, `tlp_test_packet.cpp`, compatibility fixtures | Leave frozen; not the new secure product packet family |
| One-hop v1 Relay forwarding | **P+H; full path physical F NOT established at this head** | `network_service.cpp`, `tlp_relay_forward_packet.cpp`; 4-entry volatile queue, 16-key dedupe, deterministic 1.2–4.2 s forward | Three-node source→Relay→Base physical qualification; link loss, collisions, queue load; no durable record responsibility |
| Same node originates own data and Relays | **H for B4 pure resolver/network seam, target combination NOT physically proven** | Independent `relay_forwarding_enabled` and guarded radio transition | Persisted independent service requests, combined runtime and real power/airtime test |
| Relay enabled listening policy | **P+H; M6P1 limited F** | `radio_listen_policy.cpp`, continuous RX vs windowed RX; 10 s post-TX Tracker listen default from M6P2 | True battery current, radio availability and relay-vs-sensor-TX arbitration in realistic load |
| Arbitrary sensor/secure command/v2 observation relaying | **D/N as an end-to-end approved RF forward service** | v1 POSITION Relay code and SF5B proposed v2 bounded relay wrapper | Reviewed v2 forwarding/security/TTL/loop and radio scheduling; do not forward unknown raw frames |
| Multi-hop Relay / mesh routing | **Explicit D** (v1 exactly one hop) | Stable device identity, duplicate suppression, planned TTL/security domain | Bounded multi-hop design, airtime regulation, replay/domain ownership before implementation |
| Configurable channel/SF/BW/power and airtime admission | **D**, radio values compile-time fixed; host airtime model | `radio_config.h` 869.525 MHz / SF11 / BW125 / 14 dBm; `lora_airtime.h` and M5 host tests, RF-portability ADR | No runtime persistent RF profile, duty-cycle/airtime enforcement, scale field survey or compliant multi-domain plan |
| Legacy BASE receive / serial display | **P+H+F** for direct POSITION | NetworkService base path, USB serial `BASE RX` diagnostics | Not yet full Gateway, no Edge database or authenticated custody ACK |
| Host bridge line v1 (received POSITION -> attached host) | **P+H+scoped F** (2026-10-10: six consecutive direct-path lines from one RAK receiver, checksums verified and packets decoded off-device, counter restarting at 1 after reboot; relay path and a host that stops reading not observed). The receiver prints one checksummed `BRIDGE` line per accepted POSITION with the original 34-byte packet plus path/link metadata; `BASE RX` lines alone never carried the coordinates | `bridge_frame.cpp`, `RadioManager::emitBridgePosition`, `protocol/BRIDGE_FRAME_V1.md`, `test_bridge_frame.cpp`, R2 end-to-end asserts | Output only: no custody, ACK or authentication. Host reader, backend ingest and map still missing; opaque secure kinds and BLE carriage are later slices |
| Gateway bridge (tek servis) | **Approved D, foundations A** | Gateway/Edge custody design; USB/BLE transport and other runtime services | One real LoRa-to-attached-host bridge with secure protocol, durable admission and connected-host availability; **sabit/gezer is solely deployment metadata**, not a second Gateway type or data contract |
| Gateway foreign durable custody | **A+H portable SF4B; no production physical owner/runtime** | `custody_store.cpp`, `custody_store_format.cpp`, exact-object acceptance/release tests; PR #80 | SF5 object-size/format review, nRF flash owner, authenticated custody ACK, actual Edge handoff, load and reset tests |
| Gateway→Edge→Backend ACK chain | **Architecture D; not end-to-end implemented** | `ORUN_GATEWAY_DURABLE_CUSTODY.md`, SF5G/H/I ordered plans, secure receipt foundation | Edge durable acceptance proof, app/backend ingestion and data ownership propagation; `TX_DONE` never means custody |

### 3D. Local storage, Store & Forward, security and command foundations

| Feature | Actual status / evidence | Already reusable infrastructure | Missing / next gate |
|---|---|---|---|
| Legacy local HistoryStore store-before-send | **P+H+F**; 7×4096-byte pages = **728** records, 2026-10-09 counter 715→716, `overwritten=0` | `history_store.cpp`, `position_flow.cpp`, R1/PR #58 reboot hardening | Capacity finite, full power-cut endurance/product retention policy not certified; `pending` isn't proof Gateway received anything |
| SF3 TLP v2 HISTORY_SECURE replay | **P+H and scoped F** in a provisioned development device; PR #78 | `history_store_forward_runtime.cpp`, `history_secure_crypto.cpp`, `history_receipt_admission.cpp`, `history_delivery_coordinator.cpp` | Authorized real Gateway/backend delivery and authenticated BACKEND_DURABLE closed-loop proof not shown; no provisioning UI in normal production. Intended to retire at SF5F |
| SecurityStore credential/counter/A2D replay state | **P+H, scoped F for individual storage/security probes** | `security_store.cpp`, `security_format.cpp`, `flash_mutation_gate.cpp` | Normal device remains unprovisioned unless deliberately provisioned; credentials/remote user authority not automatically active |
| Delegated gateway command security foundation | **A+H** | M7P6G KDF/nonce vectors, M7P6H protected envelope codec `tlp_v2_delegated_secure_app.cpp`; signed context/family rules | Product COMMAND/RESULT exact payload, target-side authenticated application semantics, trusted Gateway claims/authorization and field RF end-to-end not live |
| CAS / idempotent config/geofence command primitives | **A+H** persisted state tokens and store-level gates | `ConfigStore` v2, `GeofenceStore`, `ORUN_CONFIG_STATE_TOKEN_CAS_DIRECTION.md` | Authoritative command dispatch/authorization, remote mutation and RESULT; no "command transmitted = applied" claim |
| SF5 TLP v2 PRODUCT_SECURE observation/security contract | **D (approved reviewed design)** SF5A/B merged | `ORUN_TLP_V2_PRODUCT_SECURE_WIRE.md`, `ORUN_TLP_V2_TRACKER_PRODUCT_DATA_CONTRACT.md` | Product codec, protected RF activation, lifecycle, event/results and real field end-to-end not implemented |
| SF5C portable ObservationStore | **A+H** final focused audit PASS WITH FIXES, 0 BLOCKER/HIGH/MEDIUM, one doc LOW; PR #83 merged | `observation_store.cpp`, `observation_store_control.cpp`, format, host torn-write/recovery tests | Not composed with `main.cpp`; no nRF physical flash owner or live PERIODIC/EVENT cutover |
| Candidate dynamic 128 KiB shared pool | **D**, PR #89 **draft** | SF5D D1/D2 geometry preflight; single physical owner direction; 32×4096 page candidate `[0xC5000,0xE5000)` | Not 64+64 split; no SharedDurablePool implementation; sampled pages **not all blank**; recovery/GC/custody pinning, wear and owner-transition gates OPEN |
| SF5D3 serial DFU sample preservation | **Scoped F**, three full-page CRC32 samples preserved across one test SEED→VERIFY single-bank upload | SF5D test-only seed/verify + host guards; RAK bootloader version handoff 0.4.2 | Does **not** validate all 32 pages, larger production update CRCs, interrupted DFU, binary-attested installed bootloader, future LoRa FOTA, power cuts. D1/D2 OPEN |
| Unified physical flash mutation/ownership | **P+H, scoped physical probes** | `FlashMutationGate`, one arbitration boundary with BLE InternalFS, Config, Geofence, Security, History | Adding candidate pool requires reviewed single physical owner, hard bounds and async NVMC tests; must not let two stores format same page |
| Messages/private mailboxes | **D/N** | Product message identity/security/transport architecture, application request transport | No normal production MESSAGE service, encrypt/send/retry/inbox/delivery proof |
| Valve/actuator control, including Valve + Relay | **Approved architecture D; N for actual valve hardware driver/secure operation** | Nonexclusive services decision, secure COMMAND/RESULT, flash/replay and host transport foundations | Driver electrical requirements, state feedback, safe power-loss behavior, command authorization/anti-replay/idempotence and actual hardware qualification — no valve operation should be enabled before these |
| General device-to-device command/response | **A for envelope/security; D for executable product service** | Typed application requests (read-only), M7P6 crypto, CAS owner | Full authenticated target-side operation, RESULT and secure RF command family, retry and application-state validation |

### 3E. BLE, mobile, backend, operations and quality

| Feature | Actual status / evidence | Already reusable infrastructure | Missing / next gate |
|---|---|---|---|
| BLE hardware stack/advertising/bond coexistence | **P+H+scoped F** M7P7A/B/G; actual GATT verified using Android nRF Connect | `ble_admission_policy.cpp`, `ble_application_transport.cpp`, `ble_application_handoff.cpp`, framework flash arbitration | Full in-product authorization/pairing policy and field current cost not fully qualified |
| USB + BLE one typed application boundary | **P+H+scoped F** for read-only statuses | `ApplicationRequestService`, `UsbApplicationAdapter`, BLE fragmentation (M7P7D–H) | LoRa application adapter, write commands, secured app authorization and public/private-status access review |
| Android/mobile application | **N, deliberately deferred** | BLE GATT contract, typed read-only status, location/domain model, Gateway/Edge plan | Actual UI, map, permissions, provisioning, offline DB and security; no Android app in this repository tree |
| Backend / Edge server / notification service | **N, deferred until contracts/gates** | Data semantics, custody/receipt and source/observation-time contracts | Production ingestion, DB, idempotence, API, tenant auth, notifications, monitoring; no backend implementation in this tree |
| USB diagnostics, watchdog/I2C/radio recovery | **P+H; scoped F** R2/R3/R4, serial `ROLE?`, `RADIO?`, `BLE?`, `ACCEL?` etc. | `watchdog_manager.cpp`, `i2c_recovery.cpp`, `sensor_power_manager.cpp`, radio driver gate | Product-grade reset/battery/radio-health observability and all failure injection on physical hardware |
| Loop reset attribution (`HEALTH?`) | **P+H; F pending**. One retained byte (GPREGRET2) records which loop step a watchdog reset interrupted, or that a HardFault ran; an independent 2 s RTC2 tick detects a stalled loop and an idle sleep that overran. Added 2026-10-10 because a bench tracker watchdog-reset twice (about 38 and 77 min after boot). Its first reading on that tracker was `stack_free=0`: the 4 KiB loop task stack had been exhausted. SparkFun 2.2.29 `checkCallbacks()` has a 3,096-byte frame and the `GNSS FIX` printf ran inside it (about 4,130 bytes in total), so every fix overran the stack. Fix in the same change: the fix line is printed after the callback returns, and a pinned patch (`scripts/patch_loop_stack.py`) raises the core's loop task to 8 KiB. Bench evidence (below) supports this as the cause; one tracker, one night. Same change: USB status lines no longer exceed the core's 256-byte `Print::printf` buffer (APP STORAGE had ended in stack bytes on a RAK4631). | `loop_health.cpp`, `loop_health_monitor.cpp`, `gnss_manager.cpp`, `main.cpp`, `usb_application_adapter.cpp`, `scripts/patch_loop_stack.py`; `tests/r4/test_loop_health.cpp`, `tests/r4/test_patch_loop_stack.py` | Hardware so far (same tracker, 2026-10-10): before the fix `stack_free=0`; with the fix, after about 24 min of 60 s fixes, `stack_free=4640` of 8,192 B, i.e. a measured peak of 3,552 B (the 4 KiB stack would have kept only 544 B even without the print). RTC2 clock runs (`idle_longest_ms=125`). The same tracker then ran 9 h 03 min without a reset (`APP DEVICE uptime_ms_mod32=32574294 reset=0x00000004`), against watchdog resets 38 and 77 min after boot before the fix; the split `APP DEVICE` line arrived intact. Not yet observed: the retained byte surviving the bootloader, a captured stall or fault. Probe-only diagnostic lines are not audited for the printf limit |
| Battery voltage/current, low-battery policy, solar runtime | **N for actual measurement; D for power policy** | `sensor_power_manager.cpp` and GNSS/LoRa scheduled duty control; ConfigStore `battery_capacity_mah` is **metadata only** | Real ADC/fuel gauge driver, battery voltage/SoC evidence, staged policies, current instrumentation; no claimed field lifetime |
| RF/network limits and scale | **Host estimation and architecture**, no 30–50 node physical validation | M5 airtime math, finite dedupe/queue, network-domain plan | Measured duty-cycle compliance, routing, collisions, region profile; do not promise 1000-node flat RF domain |
| Local firmware update / DFU | **P+scoped F** serial USB DFU physically used, older UF2 path known | PlatformIO package/upload flow, SF5D installed handoff/CRC test | Automatic LoRa OTA/FOTA, interrupted update/rollback assurance, deployed signing/secure lifecycle not proven |
| LoRa OTA/FOTA | **Explicit D / N** | Architecture reserves update headroom; DFU preflight and version/trust boundaries | Separate secure transport, energy/airtime, rollback and capacity milestone; not bundled into SF5D3 |
| Host regression/sanitizers/fuzz/CodeQL | **H** full host suite **EXIT 0** on 2026-10-09 user log; production build SUCCESS; CodeQL and fuzz setup present | `firmware/tests/run_host_tests.sh`, `tests/fuzz`, `tests/codeql`, startup harness, source-contract guards | A green build is not all CodeQL analyses or physical tests; individual workflow failures need explicit triage, never relabel FAIL as PASS |

## 3F. Power and sleep: important current sensor-only gap (verified from `main` source)

**Do not state that every node with Relay=OFF and Gateway=OFF already
sleeps in production.** The target service-driven rule is valid; the
existing implementation is partly dependent on legacy role behavior.

| Case | Current `main` evidence | Result |
|---|---|---|
| Legacy `TRACKER` with forwarding OFF | `RadioManager::desiredListenPolicy()` resolves `kWindowed`; `serviceWindowDeadline()` calls `Radio.Sleep()`; local TX wakes radio; `kWindowedRxAfterTxMs=10000` | **LoRa RADIO sleeps** after a bounded 10-second RX window; M6P1/2 have scoped real RAK evidence |
| Legacy `RELAY` or application-receiving `BASE` | `resolveRadioListenPolicy(relay_running, receives_application, role_transition_pending)` returns `kContinuous` | LoRa stays **continuous RX** (except own TX); normal for Relay/receiver availability |
| **GNSS-absent future fixed sensor, Relay OFF, Gateway OFF** | `RoleController::updateAutomatic()` currently maps **missing GNSS to BASE**; `BASE` sets `receive_application_position=true` | **BUG/GAP against target:** LoRa would use continuous RX despite no Relay/Gateway requested. A service-driven RX requirement must replace legacy role-based admission; absent GNSS must not turn a fixed sensor into a receiver |
| Normal nRF CPU | `PowerManager::idle()` calls `delay(10)`; FreeRTOS tickless idle may wait when no task is runnable | Cooperative low-power idle, **not guaranteed SYSTEM OFF/deep sleep** or a measured battery lifetime |
| GNSS peripheral after acquisition | `GnssManager::enterLowPower()` conditionally releases `SensorPowerManager::kGnss` 3V3_S rail | Peripheral supply can turn off between acquisitions, except explicitly preserved continuous GNSS policy |
| BLE availability | bounded advertising window, connected sessions may remain; independent BLE radio | BLE state can impact whole-board consumption; not evidence the entire node sleeps |

**Design prerequisite before sensor-only devices ship:** `RxListenRequired`
must derive from independently requested/effective **Relay, Gateway
RF receive/bridge, actual downlink rendezvous and any explicitly committed
receiver service**, not from GNSS presence or `NodeRole::kBase`. When no
continuous receiver service is enabled, keep bounded TX/response/listen
windows, then radio sleep. A legitimate Gateway/Relay must not be silently
suspended by this fix. User configuration remains authoritative after
persistent-service migration. Prove startup with **no GNSS, Relay OFF,
Gateway OFF**, with timer-based sensor sampling/own packet TX, LoRa
`WINDOWED→ASLEEP→TX→WINDOWED→ASLEEP`, BLE bounds, zero unauthorized
Gateway custody ACK, and measure current before estimating run time.

No physical fixed-sensor radio cycle was tested during this inventory
update; no firmware changed.

## 3G. Persistent settings across reboot and firmware update — product requirement

**User-required outcome (2026-10-09):** After a normal restart, watchdog
reset, external-power disconnection/reconnection, or **supported, ordinary**
firmware update, previously committed valid ORUN configuration must remain
intact and active **without a second setup**. This includes *future* independent
requested services (Konum Takibi, Sensör Raporlama, Relay, Gateway, Vana
Kontrolü), location source, device installation/calibration and power/report
policy only **after each corresponding versioned semantic schema is actually
implemented**. Preserving a configuration value must not by itself grant
security authorization, valve authority, or keep any hazardous actuator
energized across reboot. Gateway/Relay enablement and continuous-RX power
policy must be explicit, durable user intention, not automatic GNSS-based role.

**Current, demonstrable scope:**
- `ConfigStore v2` at `[0xE9000,0xEB000)` implements two-page crash-safe
  commit/recovery with config state token. Its **only** semantic values are
  `tracking_interval_seconds` and `battery_capacity_mah` (the latter is
  configured metadata, not a battery charge measurement). Schema v4
  (2026-10-10, host-tested, physical validation pending) adds the requested
  service intent to the same record; nothing reads or writes that intent at
  runtime yet.
- The normal firmware starts `ConfigStore` and reads persisted values at boot.
  Since 2026-10-10 a local USB writer exists for the **tracking interval
  only** (`APP INTERVAL <seconds>`, 60 s floor): `ConfigMutationOwner`
  serializes the change, ConfigStore commits it, the runtime adopts it and a
  typed result is printed (**P+H+scoped F**, 2026-10-10 on one receiver:
  APPLIED, read back as stored, retained across a serial DFU of the same
  image plus reboot, then restored; not observed: a tracker changing cadence,
  a save with BLE connected, power loss during a save started this way).
  **There is still no
  writer for service settings and no authenticated BLE/LoRa writer.** Manual
  USB `ROLE` override is still RAM-only; that selected role does **not**
  survive reset.
- The independent ConfigStore v2 physical qualification PASS covers
  **specific between-flash-operation** reboot/cut points on one device.
  It is not analog brownout, an interrupted live SoftDevice write, or
  an arbitrary erase/program brownout proof.
- SF5D3 sampled serial DFU retention PASS covers **three sample pages**
  in the *candidate* `[0xC5000,0xE5000)`, not ConfigStore
  `[0xE9000,0xEB000)` itself, all data regions, all update
  modes or schema migration. SF5D D1/D2 remain OPEN.

**Mandatory implementation/upgrade acceptance contract:**
1. Design one durable desired-state owner and explicit versioned schema.
   Preserve configured service intent **independently** from capability
   probes or transient effective service failure. Use atomic whole-config
   (or equivalent reviewed transactional) commits with token/CAS semantics.
2. **Restart**: save/verify `A` → restart/watchdog/power cycle →
   recover exact semantic `A` + valid token, ensure effective status
   reflects capabilities, and no unnecessary write/erase occurs at boot.
   Report committed vs pending change separately; never promise uncommitted
   values survive.
3. **Firmware update**: backup/baseline snapshot and per-owned-partition
   canaries on a development device → supported serial/USB DFU of
   production-size image → reboot → exact config/token recovery and
   no unintended overwrite of History/Geofence/Security/BLE or future
   Observation/Custody. Repeat for each **actually supported** update
   transport; DO NOT conflate sampled SF5D3 pages with full protection.
4. **Schema evolution**: explicitly reviewed forward/backward compatibility
   and power-cut-safe migration; preserve known values, supply defined
   defaults only for *new* fields. If unknown newer/legacy/corrupt state
   cannot be interpreted safely, **fail closed** and report maintenance;
   never silently factory-reset/format/reinitialize an ambiguous page.
   Downgrade/rollback safety requires its own tested policy, not a promise.
5. **Interrupted save / DFU**: after tested failure paths, either the
   last committed valid state is retained or the node enters an
   observable maintenance/safe state. Security counter, anti-replay,
   actuator safety, PIN/bond and stored observations are distinct from
   ordinary editable settings and have separate preservation rules.
6. **Status/UI**: expose `saved` vs `requested` vs `effective`,
   pending/failed mutation, schema version and error reason without
   presenting old volatile `ROLE?` as permanent config. A user sees
   only the independent services and the single **Gateway** label.

**Do not say “firmware updates already guarantee all settings survive.”**
That contract requires the release/update path, schema migration and
physical evidence above before it can be declared complete. No flash
changes or device writes are authorized by this documentation.

## 4. Deferred items: does the prerequisite architecture exist?

| Planned item | Is groundwork already there? | Real dependency / acceptance criterion | Priority |
|---|---|---|---|
| Persisted independent services and editable profiles | **Yes — B4 resolver and ConfigStore v2** | Review config v3/new authority/format migration and reconnect/rollback; stop deriving production intent from AUTO GNSS role; target-side status and safe rollback | **NOW — separate bounded slice** |
| Sensor + Relay, Tracker + Relay in one node | **Yes — B4 resolver/forwarder** | Persisted enabled flags, physical concurrent own-report+forward/RX, measured energy/airtime | **NEXT** |
| Gateway + Tracker shared durable 128 KiB | **Partial — two portable stores and D1 sampling** | D1 full owner/DFU; D2 single shared journal and custody invariants, host crash fault injection | **NOW design/review; writes only after gates** |
| Real periodic activity/telemetry product records | **Yes — M6 activity capture and SF5A/C contracts** | Define period summary, sensor cadence, secure PRODUCT_SECURE, ObservationStore cutover | **SF5F, not before SF5D/E** |
| Geofence editor, remote protected update and alarm | **Yes — geometry, store, operational runtime, token security** | UI editor; authorized CAS writer; event persistence and secure RF EVENT+backend | **NEXT after command/wire security** |
| Additional environmental sensor modules | **Partial — hardware discovery and sensor ownership rule** | Choose actual sensor and circuit, driver, typed report, power tests; no general plug-in engine needed now | **LATER by concrete hardware** |
| Vana/sulama actuator + Relay | **Architecture yes; actuator driver/security no** | Electrical fail-safe, local override/feedback, scoped signed/authenticated anti-replay command and physical result; RF Relay remains independent | **LATER gated; safety-critical** |
| Full Gateway and Edge offline sync | **Yes in design/portable store; no connected bridge** | Product secure format; persistent foreign custody; authenticated exact-object ACK; durable Edge acceptance; real connected USB/BLE host | **SF5G/H** |
| Mobile map, backend, notifications | **Semantic foundations yes; software N** | Product packet, trust, gateway/Edge ingestion; offline data model | **SF5I** (some UI prototyping can be independent once status contract fixed) |
| Multi-hop, adaptive RF and large fleet | **Limited one-hop and airtime models** | Domain IDs, loop/TTL admission, regulatory airtime accounting, measured load | **LATER based on field requirement** |
| LOST, FREE_GRAZE and richer animal behavior | **Location/geofence/activity foundations** | Authenticated contact, classifications calibrated with labeled field data, safe event lifecycle | **LATER; `NEAR_FENCE` optional** |
| Battery/fault/power optimizations | **Power ownership and diagnostics primitives** | Actual battery voltage/current and TTFF/radio energy measurements after battery installed | **BEFORE battery-life promises / field signoff** |
| LoRa FOTA | **DFU/headroom direction only** | Signing, transport cost, interrupted rollback, update-preserved storage | **LATER separate major milestone** |

## 5. Practical next work and guardrails

1. **Do not create fresh Tracker/Sensor/Relay/Gateway architecture classes.**
   Existing PR #42, `AGENTS.md`, B4 and M6 semantics already own that idea.
   User-facing product terms must follow §1; old C++/TLP names only change after
   a compatibility/migration gate.
2. **PR #89 remains the separate SF5D4 flash design review.** It must not be
   merged as implementation, and must not write to candidate flash without
   D1/D2 owner acceptance.
3. **Next self-contained coding target:** independent service intents and
   persisted requested/effective statuses. First write a diff/compatibility
   contract for legacy bootstrap, GNSS absence, `ConfigStore` format and
   authenticated/USB/transport writers. Explicitly show no silent remote valve
   command authority, no source ownership migration and no hidden service
   power toggles.
4. Parallel read-only planning: one bounded `SharedDurablePool` capacity and
   torn-update specification derived from SF5C and SF4B. No allocator
   implementation until GC/eviction/custody invariants are reviewed.
5. **Physical qualification:** first run one normal combined tracking+relay
   device, then three-radio forwarded-path E2E if needed, and measure mA/airtime
   when instrumentation available. The two current RAKs prove **direct** P2P,
   not all three-hop/actuator/Gateway cases.
6. Explicitly record separate future hardware slices for chosen environmental
   sensors and a real valve board — no fake generic drivers or GPIO actuator
   permissions without electrical specification.
7. **Never delete existing source/tests** because a branch is "deferred".
   Remove only redundant newly introduced architectural duplication; retired
   legacy wire/test fixtures remain compatibility guards.

### Current machine evidence and limits

On **2026-10-09**, owner provided:
```text
host: HOST_EXIT=0
production build: SUCCESS
upload: Device programmed. SUCCESS 34.04 s
GNSS FIX ttff=109021ms ... sats=4 hdop=2.41
TX POSITION sequence=27138
STORAGE appended records=715 overwritten=0 pending=715
GNSS FIX ttff=2974ms ... sats=4 hdop=3.03
TX POSITION sequence=27139
STORAGE appended records=716 overwritten=0 pending=716
```
This **does not show** the second radio's RX on that same 2026-10-09 monitor,
an authenticated `BACKEND_DURABLE` receipt, a foreign custody ACK, a valve
command, or an end-to-end secure observation delivered to a backend.

### Source navigation / repeatable audit method

- List all committed code: `git ls-files firmware/include firmware/src firmware/tests firmware/platformio.ini`.
- Read present/absent executables: `firmware/src/main.cpp`, `runtime_config.cpp`,
  `node_role.cpp`, `network_service.cpp`, `radio_manager.cpp`,
  `application_request.cpp`.
- Verify durable state and non-overlapping layouts:
  `storage_config.h`, `flash_mutation_gate.cpp`,
  `history_store.cpp`, `config_store.cpp`, `geofence_store.cpp`,
  `security_store.cpp`, `observation_store.cpp`, `custody_store.cpp`.
- Verify real target build and tests on local developer host (not executed
  during writing this document): `bash firmware/tests/run_host_tests.sh`
  and `cd firmware && pio run -e rak4630`.
- Link each claim to the **specific** milestone and hardware evidence;
  do not conflate source contracts, test-only probes or earlier audit
  checkpoints with current production behavior.

**Update rule:** whenever a merged PR changes a row, amend this inventory
with (1) SHA/PR, (2) source/test evidence, (3) physical evidence or explicit
not-tested, (4) newly closed/open gates. No copy of an old "NOT IMPLEMENTED"
claim without checking current `main` first.
