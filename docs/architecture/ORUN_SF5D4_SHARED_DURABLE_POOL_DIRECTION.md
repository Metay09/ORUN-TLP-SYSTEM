# ORUN SF5D4 — Shared Durable Pool for Combined Tracker + Relay + Gateway (DRAFT)

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

## 1. Product decision and separation

One ORUN physical node may simultaneously offer:
- **Tracker**: originates its own observations and location;
- **Relay**: forwards authorized/eligible RF envelopes without durable
  possession; forwarding does not imply custody or an ACK;
- **Gateway**: bridges RF toward an Edge/phone/USB/IP connection and optionally
  accepts **durable custody of foreign** logical observations.

These are separately enabled *capabilities and services*, not mutually
exclusive permanent device roles. Hardware/power/half-duplex SX1262 RF duty
and bounded listening policy still determine whether each service is actually
available. Gateway != Edge: a RAK4631 alone has no general IP/Wi-Fi Internet
connection; local USB/BLE transport may hand data to a separate phone/host.

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

## 3. Candidate storage ownership

Introduce one **SharedDurablePool** *logical physical owner*, controlling
all format, page allocation, flash append, garbage collection, erase,
readback, recovery and resource accounting within the proposed region:

```
TRACKING observation service ----> own-observation logical view ----\
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
  time and integrity metadata; bounded oldest-first eviction permitted under
  explicitly recorded local-capacity loss. No silent reinterpretation as
  successfully uploaded data.
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
9. Logical `Tracker`, `Relay` or `Gateway` enablement must never change
   flash ownership, erase, reformat, or orphan a previously ACKed object.

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

This is a **design direction**, not a claim that combined Tracker/Relay/Gateway
or pooled flash works today; no code is implemented and no physical flash is
allocated by this proposal. Role coexistence in other projects does not prove
ORUN's exact-object custody, offline handset delivery, scale, range, power
budget or data retention.
