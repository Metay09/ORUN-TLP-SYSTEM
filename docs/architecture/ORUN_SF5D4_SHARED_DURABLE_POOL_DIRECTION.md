# ORUN SF5D4 — Shared Durable Pool for Composable ORUN Nodes (DRAFT)

Status: **PROPOSAL FOR INDEPENDENT REVIEW, DESIGN-ONLY**.
Owner decision, 2026-10-09: favor **dynamic common management** of the
single candidate 128 KiB region, **not** a fixed 64/64 KiB split.
No firmware implementation, flash allocation, write authorization, wire format
or production cutover is granted by this document. **SF5D D1 and D2 remain OPEN.**

Baseline: `main@336ab8f13f1f2ced4eb18034e4a05ac131e0f4e1`
(SF5D3 one-page SEED / three-page CRC32 serial DFU physical qualification).

Canonical directions:
- `docs/architecture/ORUN_NODE_CAPABILITIES_AND_PROFILES.md` — single ORUN node, independently enabled sensing, location, actuation, relay and gateway; user-facing naming.
- `docs/architecture/ORUN_PRODUCT_SYSTEM_ARCHITECTURE.md`
- `docs/architecture/ORUN_TLP_V2_PRODUCT_OBSERVATION_STORAGE_CUTOVER.md`
- `docs/architecture/ORUN_GATEWAY_DURABLE_CUSTODY.md`
- `docs/milestones/SF5D.md`
- `docs/milestones/SF5C.md`

## 1. Product decision and separation

An **ORUN node** has one stable device identity. Its independently requested
services may include **GNSS/Location tracking, sensor sampling/reports,
valve/actuator control, Relay forwarding, Gateway bridge and Gateway durable
custody**. Which services can effectively run depends on attached/validated
hardware, authorization, power, RF and explicit safety policy. See
`ORUN_NODE_CAPABILITIES_AND_PROFILES.md` for the normative proposed
terminology, deployment examples and actuator-safety boundaries.

A physical node can originate its own location/sensor observations, operate
an authorized local actuator, **and** forward other nodes' RF traffic; a
fixed sensor/valve node may optionally be a Relay. Animal collars keep Relay
OFF by default. A Gateway can be fixed or mobile without requiring different
gateway semantics; both depend on an approved connected phone/host/Edge for
upstream connectivity.

- **Own-data service**: creates this node's location, sensor, activity or
  eligible equipment observations; no GNSS prerequisite for ordinary sensors.
- **Relay**: forwards approved RF frames without taking durable possession.
  Forwarding neither stores foreign custody nor establishes a custody ACK.
- **Gateway bridge**: transfers traffic via external phone/USB/BLE/IP-capable
  host; its durable-custody capability is separately authorized and proven.
- **Actuator service**: may coexist with Relay and sensors, but independent
  hardware interlocks, secure commands and verifiable outcomes are mandatory.
  Neither Relay nor Gateway implies authority to operate a valve.

No service is an exclusive permanent device type. Hardware/power/half-duplex
SX1262 RF duty and bounded listening policy still determine actual runtime
availability. The current legacy production role-to-service projection is not
yet replaced; this is a proposed migration design, not a running combination.

**Decision direction:** one common, bounded physical durable-pool owner,
not two isolated 64 KiB partitions and not two independent 128 KiB promises.
Any split into metadata/scratch/recovery pages is **mechanical overhead**, not
a fixed per-service capacity partition. Usable payload capacity is lower than
128 KiB and not yet measured.

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

## 2A. Selectively enabled Tracker + Relay on the same device

**Current implementation truth (source and operator evidence, 2026-10-09):**
- The production `RequestedConfig` and `EffectiveConfig` distinguish
  `tracking_enabled` and `relay_forwarding_enabled`; the `RadioManager`
  independently implements guarded `setRelayForwardingEnabled()`.
- **Production requested settings are still derived from legacy `NodeRole`**:
  `TRACKER` = GNSS tracking on, relay off;
  `RELAY` = tracking off, relay on;
  `BASE` = local application receive, relay off.
  There is **no independently usable, persisted Tracker+Relay switch** on the
  normal production configuration surface today.
- The frozen v1 relay path forwards accepted **direct POSITION** frames in
  a bounded 4-entry RAM queue with 16-key duplicate suppression,
  deterministic 1,200–4,200 ms delay, one hop and no nested relay packets.
  This is **opportunistic volatile forwarding**, not foreign durable custody.
  It does not ACK source records, nor can the Relay promise post-reset delivery.
- Relay enabled imposes **continuous LoRa RX between local transmissions**,
  vs the battery-optimized Tracker's scheduled receive windows. Because SX1262
  is half-duplex, incoming packets can be missed during the node's own TX;
  contention, coverage, current draw and heat/energy must be measured.
- Available operator records prove direct Tracker→Base on two RAK boards,
  not three-node Tracker→Relay→Base nor simultaneous own-position TX plus
  foreign relay TX on one physical RAK board.

**Required next-slice behavior:**
1. Keep relay **OFF by default** for animal collars. Explicitly select devices
   with suitable power source/charging/role (powered fixed nodes, vehicle,
   solar or independently reviewed collar exceptions). No silent auto-enable.
2. Give tracking and forwarding **independent persisted service requests**,
   observable effective state, and documented power-policy conflict handling;
   enabling forwarding must not silently disable own tracking.
3. Keep relay traffic priority, dedupe, queue/airtime admission and bounded
   listening requirements explicit. Preserve the legacy one-hop v1 contract;
   independently design forwarding for the v2 product and secure object
   families before relying on it.
4. Test both simultaneously: (a) tracker keeps acquiring and persisting GNSS
   while relay remains enabled; (b) receive/queue/transmit another source's
   position; (c) downstream independently receives `path=RELAY`; (d)
   lost packets, duplicate/collision behavior, battery current, resets,
   TX/RX contention and mode switches are recorded. A two-board test can
   demonstrate local combined transmit/forward diagnostics, but complete
   source→combined→receiver RF end-to-end ordinarily needs a third radio.
5. **Do not consume SharedDurablePool merely to forward**. Persist foreign
   packets only when a separate authenticated Gateway durable-custody service
   has accepted responsibility under the reviewed custody contract.

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

This is a **design direction**, not a claim that combined GNSS/sensors/valve/
Relay/Gateway or pooled flash works today; no code is implemented and no
physical flash is allocated by this proposal. Valve/actuator control is
subject to its own hardware and security qualification. Role coexistence in other projects does not prove
ORUN's exact-object custody, offline handset delivery, scale, range, power
budget or data retention.
