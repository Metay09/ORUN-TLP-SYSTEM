# ORUN SF5D4 — Dynamic Shared Durable Pool for an Already-Modular ORUN Node (DRAFT)

Status: **PROPOSAL FOR INDEPENDENT REVIEW, DESIGN-ONLY**.
Owner decision, 2026-10-09: favor **dynamic common management** of the
single candidate 128 KiB region, **not** a fixed 64/64 KiB split.
No firmware implementation, flash allocation, write authorization, wire format
or production cutover is granted by this document. **SF5D D1 and D2 remain OPEN.**

Baseline: `main@336ab8f13f1f2ced4eb18034e4a05ac131e0f4e1`
(SF5D3 one-page SEED / three-page CRC32 serial DFU physical qualification).

Canonical directions:
- `docs/architecture/ORUN_PRODUCT_SYSTEM_ARCHITECTURE.md`
- `docs/architecture/ORUN_TLP_V2_PRODUCT_OBSERVATION_STORAGE_CUTOVER.md`
- `docs/architecture/ORUN_GATEWAY_DURABLE_CUSTODY.md`
- `docs/milestones/SF5D.md`
- `docs/milestones/SF5C.md`

## 1. Existing product decisions — do not redesign

**The universal ORUN node was ALREADY approved and merged before this PR.**
`AGENTS.md` (see `Universal Firmware`, `Radio Architecture` and `Power Policy`)
and `docs/architecture/ORUN_PRODUCT_SYSTEM_ARCHITECTURE.md` are canonical:
a node's one stable identity, physical modules/capabilities, independent
services, power policy, transport and editable profile are distinct.
The 2026-09-26 architecture PR #42 formalized the existing product decision.

Existing approved combinations include own GNSS/sensor measurements and
LoRa Relay on one node, a fixed sensor/actuator/valve node that also Relays,
and one **Gateway** service with optional other services. "Sabit"/"gezer"
is only deployment metadata; do **not** name different Gateway types,
profiles, flash owners or protocols. The operator must see one `Gateway`
control alongside `Konum Takibi / Sensör Raporlama / Vana Kontrolü / Relay`.
Do not present legacy `TRACKER/RELAY/BASE` as competing product choices:
their current source symbols/USB compatibility are temporary internal
adapters until an explicitly tested configuration cutover. Especially,
legacy `BASE` must **never be relabeled** as an already working Gateway.
Animal collars default to Relay OFF to protect battery; Relay enabled
requires continuous LoRa RX between local transmissions.

**Do not reimplement or re-freeze these as SF5D4 scope.** Under an approved
separate feature milestone, implement missing generic sensor/actuator drivers
and safe, authenticated actuator control; this PR must not invent an
unaudited valve command format or imply a valve already works.

The **new question exclusively in SF5D4** is how the candidate *one physical*
128 KiB flash region can have a safely reviewed single owner that dynamically
serves this node's own observations and separately accepted foreign Gateway
custody without overlap, fixed 64/64 quotas or false dual-capacity guarantees.

## 2. External project comparison: role coexistence only, NOT custody proof

- Meshtastic: ordinary CLIENT nodes originate GPS/telemetry and intelligently
  rebroadcast eligible mesh messages. Optional MQTT uplink can bridge to IP
  through suitable network hardware or an external client proxy. Their
  published device-role and network descriptions do **not** establish ORUN's
  durable exact-object custody-ACK semantics.
  https://meshtastic.org/docs/configuration/tips/
  https://meshtastic.org/docs/configuration/module/mqtt/
- MeshCore: Companion and dedicated Repeater firmware exist; newer
  Companion Repeat enables limited forwarding while acting as a client,
  with airtime/frequency constraints. Full combined managed repeater
  functionality remains separately evolving. No claim of ORUN-style
  persistent foreign-custody guarantee follows.
  https://github.com/meshcore-dev/MeshCore/discussions/1650
  https://github.com/meshcore-dev/MeshCore
- LightLoRaAPRS: one firmware supports selectable Tracker, iGate and
  Digipeater capabilities. Configurable switching does not prove every
  function can run simultaneously, and APRS iGate acceptance is not
  authenticated durable custody.
  https://github.com/lightaprs/LightLoRaAPRS
- APRS-ESP: one APRS firmware combines Tracker, Digipeater and iGate
  settings; it is an ESP32 AFSK1200 APRS project, **not** a like-for-like
  SX1262/RAK4631 ORUN architecture or flash-safety precedent.
  https://github.com/erstec/APRS-ESP
- littlefs: an existence proof of embedded power-loss recovery, common
  block allocation and wear management; ORUN must **evaluate**, not
  automatically adopt it, because the current flash ownership, preserved
  Security/Config/BLE partitions and durable-custody semantics are distinct.
  https://github.com/littlefs-project/littlefs

Learn from role coexistence and disciplined page management. Do not copy
generic mesh flooding, APRS acknowledgment semantics, or a foreign on-flash
format into ORUN's TLP v2 and local flash without review.

## 2A. Existing code inventory and actual gaps (read-only GitHub main audit)

| Area | Already implemented | Gap — do not imply complete |
|---|---|---|
| Independent feature resolver | `runtime_config.h/.cpp`: `RequestedConfig(tracking_enabled, relay_forwarding_enabled, location_source)`, `EffectiveConfig` with reasons; `firmware/tests/b4/test_b4.cpp` explicitly tests **tracking+relay both ENABLED** | Production `main.cpp::resolveRuntimeConfig()` still projects legacy `NodeRole` into requested settings |
| Relay | `NetworkService::setRelayForwardingEnabled()`, `RadioManager::setRelayForwardingEnabled()`, source/sequence duplicate suppression, 4-entry RAM queue, 1,200–4,200 ms scheduling; source and host tests | Current v1 forwards direct POSITION only (one-hop); no full simultaneous GNSS+Relay physical evidence, no generic v2/sensor/command-forward proof |
| GNSS/sensor | Production GNSS/HistoryStore/TX proven on RAK-1; RAK1904/LIS3DH capability probe, `AccelerometerManager`, `ActivityCapture`, activity window code/tests | Do not confuse this with arbitrary plug-and-play environmental sensor data or a finished permanent sensor-report runtime |
| Config persistence | `ConfigStore` v2 crash-safe tokenized A/B persistence | Current `config_format::Config` stores only `tracking_interval_seconds` and `battery_capacity_mah`; independent service intentions/profiles are **not persisted** |
| Valve/actuator | Existing command security direction and limited application/delegated security foundation | No production valve driver, output/interlock/feedback owner or secure valve-application command handler demonstrated; application request service currently supports read-only GET_* |
| Gateway/custody | Portable `CustodyStore` foundation and documented backend/Edge handoff semantics | Full Gateway custody runtime and combined physical owner not activated |
| Shared flash | SF5C portable ObservationStore; SF5D3 three sample page CRC retention across one serial DFU | No physically allocated or audited 128 KiB shared pool; D1/D2 **OPEN** |

Architecture stays canonical in `AGENTS.md` and
`docs/architecture/ORUN_PRODUCT_SYSTEM_ARCHITECTURE.md`.
SF5D4 should reuse these contracts rather than duplicate the universal-device
specification in an additional product architecture file.

## 3. Candidate storage ownership

Introduce one **SharedDurablePool** *logical physical owner*, controlling
all format, page allocation, flash append, garbage collection, erase,
readback, recovery and resource accounting within the proposed region:

```
OWN data (GNSS/sensors/eligible equipment events) ----> own-observation view ----\
                                                                    > SharedDurablePool
GATEWAY custody service ---------> foreign-custody logical view --/        |
                                                                             +-- one bounded flash backend
RELAY forwarding ----------------> RF only; no custody admission            +-- FlashMutationGate
```

The existing portable ObservationStore and CustodyStore are **not** authorized
to mount and program the same region. Reuse their independently verified
semantic record / lifecycle logic where appropriate, but choose and audit
a *single physical format/allocator/recovery owner* before any runtime
composition. Two `FlashBackend` instances with overlapping pages are
**forbidden**, even if writes are serialized.

Two logical record classes (not separate partitions):
- `OWN_OBSERVATION`: stable source/record identity, original observation
  time and integrity metadata, including this node's eligible GNSS, movement,
  environmental sensor and *reported* equipment observations. Bounded
  oldest-eligible-first eviction applies only to ordinary finite-retention
  observations with explicitly recorded local-capacity loss. Do not silently
  reinterpret loss as successful upload. **Actuator command authority, replay
  state, pending operations and safety-critical verification/audit data are
  not automatically evictable observations**; assign and review their
  ownership/retention separately before implementation.
- `FOREIGN_CUSTODY`: stable originating device/record/exact protected-object
  identity and opaque payload, custody/admission lifecycle, downstream durable
  acceptance. Accepted custody is **pinned** against capacity eviction until
  authenticated matching durable release is committed. Duplicate arrivals
  must not multiply retained objects or create multiple responsibilities.

Do not grant a local custody ACK on mere RF reception, queue placement,
RAM state, `TX_DONE`, socket success, or incomplete flash program.
For the SF5 protected path, obey the current conditional exact-object
authenticated Edge-durable-accept / local-durable-commit+readback rule; the
Edge fast path remains forbidden until separately implemented/authenticated.

## 4. Dynamic shared-capacity policy (candidate)

1. The common allocator uses any eligible free capacity for either class,
   subject to non-evictable custody and recovery/scratch reserve. **No 64/64
   quotas, no forced role-dependent formatting**.
2. Reserve the physical spare/control space needed for safe copy/compact,
   page-level erase and power-cut recovery. Exact reserve and data geometry
   remain **OPEN** pending analytical capacity/wear and injected failures.
3. If pressure occurs, retire only the **oldest eligible own observations**,
   explicitly record the scope/count of irreversible retention loss and
   respect the existing tracker policy.
4. Never discard accepted, un-released `FOREIGN_CUSTODY` merely to make
   room for own records, different foreign records or role changes.
5. If a new foreign object's verified durable admission cannot be completed
   without touching pinned custody or compromising recovery reserve, **reject
   admission and withhold custody ACK**; upstream retains responsibility.
6. If an own observation cannot be safely written, report the explicit
   capacity-loss condition; never claim persistence or delivery.
7. Background cleanup is allowed only under bounded energy/flash arbitration
   policy. Page relocation must retain pinned objects across every possible
   reset; old source pages cannot be erased before replacement is
   authoritative and recoverable.
8. Fairness: reject unbounded foreign-origin floods before committing data;
   a simple pooled journal is not permission for a single attacker/device
   to crowd out the node or repeatedly wear flash. Admission quotas, auth,
   replay, peer bounding and flash-wear limits remain separate required gates.
9. Any sensor, Location, actuator, Relay or Gateway service toggle or profile
   change must never implicitly change physical flash ownership, erase,
   reformat, or orphan a previously ACKed object. Actuator safety/security
   state may not be reclassified as disposable telemetry.

## 5. Size and installed-device caveats

The candidate interval `[0x0C5000,0x0E5000)` is 32 x 4096-byte pages.
The SF5D3 physical experiment saw:
- `0x0C5000` and `0x0D5000` **already contained unknown nonblank bytes**.
  Those page contents were unchanged across one serial DFU.
- `0x0E4000` was blank, then held a 16-byte explicit test marker;
  all three whole-page CRC32 values survived the SEED -> VERIFY DFU.

**This region is not certified blank/unowned.** No autoformat or overwrite,
including of the test marker, is authorized by SF5D3. The user's agreement
to lose development *test records* is not a production data-loss policy and
is not a permission to overwrite unidentified flash owners, bootloader,
SoftDevice, Geofence, Security, Config, BLE or legacy History.

The three-page single-update test does NOT prove all 32 pages, production-size
images, interrupted DFU/rollback, an installed binary-attested bootloader, or
flash endurance. **D1 remains OPEN.**

## 6. Mandatory implementation/review gates

**D2-A: logical contract and bounded capacity model (next slice)**
- Model records and immutable identities; pin/release state transitions;
  exact-object custody and role changes independently of flash layout.
- Quantify worst-case record counts, variable-size page fragmentation,
  GC spare-space, endurance and time/energy budgets. No two independent
  128 KiB claims.
- Decide whether a reviewed shared journal is viable on 32 pages or if
  external storage / a deliberately limited combined mode is necessary.
- Assess flash owner against portable SF5C / SF4B contracts and frozen TLP
  wire bytes; do not silently replace them.

**D2-B: portable journal and adversarial fault tests (no hardware writes)**
- One physical owner, two logical views, bounded memory/index and
  single `FlashMutationGate` authority. No unbounded RAM allocations.
- Crash/fault injection before/after every program, async completion,
  header/commit/control tear, relocation and erase, including every
  4096-byte erase-prefix scenario where relevant.
- Prove pinned-custody retention and **never ACK without durable acceptance**
  under full pool, duplicates, reboot/replay, relocation and mode changes.
- Prove oldest-first own-drop transparency, capacity/rejection diagnostics,
  corrupted/unsupported-format fail-closed recovery, flash wear/GC metrics.
- Host sanitizers, source layout/ownership guards and production build must
  pass; runtime must remain unconnected in this slice.

**D1 and physical admission before production**
- Installed DFU erasure-map, normal application growth, interrupted updates,
  actual 32-page behavior, existing owner overlap and initial clean-state
  ceremony must be independently qualified without bootloader/SoftDevice
  changes or preemptive erase.
- Physical backend and brown-out/SoftDevice async behavior, single producer
  + simultaneous relay/RX/bridge schedules, battery costs and end-to-end
  durable exact-object handoff are separate PASS requirements.
- No new custody ACK, automated migration, production partition or nRF
  write is enabled until all applicable security/storage/recovery gates close.

## 7. Explicit non-claims

The existing B4 combined-service **host resolver** is not physical proof of a working simultaneous GNSS+Relay node. This document does not implement actuator controls, sensor adapters, service persistence or the SharedDurablePool. No existing device flash is allocated, erased or reformatted. SF5D D1/D2 remain OPEN. Role coexistence in other projects does not prove
ORUN's exact-object custody, offline handset delivery, scale, range, power
budget or data retention.
