# ORUN TLP v2 Product Observation / Durable Storage Cutover Direction

Status: **OWNER-APPROVED PRODUCT DIRECTION — DESIGN / MIGRATION CONTRACT ONLY. NO
PRODUCTION RUNTIME, WIRE-BYTE, FLASH-PARTITION OR PHYSICAL-QUALIFICATION CLAIM IS
MADE BY THIS DOCUMENT.**

Baseline: `main@89492c8d87b42fe1c9bd6bb334b41812640db74b` (SF4B merged).

This document records the owner-approved direction reached after SF4A/SF4B for
moving the product from the legacy POSITION-centric History path to a TLP v2
product-observation path with a larger tracker durable store.

It must be read together with:

- `docs/architecture/ORUN_PRODUCT_SYSTEM_ARCHITECTURE.md`
- `docs/architecture/ADR_M7P6_SECURITY_ARCHITECTURE.md`
- `docs/architecture/ORUN_TLP_V2_DELEGATED_COMMAND_SECURITY_DIRECTION.md`
- `docs/architecture/ORUN_CONFIG_STATE_TOKEN_CAS_DIRECTION.md`
- `docs/architecture/ORUN_GATEWAY_DURABLE_CUSTODY.md`

The design rule remains:

> Future fruit determines the trunk, but only today's required branch is
> implemented.

---

## 1. Product objective

The tracker must preserve the bounded, meaningful product observations that the
user would later care about when no Gateway/Edge is reachable, without writing
raw high-rate sensor streams to flash.

The normal tracker record is a report-period observation:

```text
PERIODIC_OBSERVATION
├─ Location
├─ Activity summary covering the same report period
├─ Battery / bounded telemetry
└─ Bounded health summary
```

Asynchronous product facts are separate records:

```text
EVENT
COMMAND_RESULT
```

Human MESSAGE traffic is not part of the animal-tracker durable-data profile.
Messaging remains an independent application service used by products/endpoints
that actually own messaging capability.

Raw accelerometer samples, raw GNSS/NMEA streams and unbounded debug logs are not
normal durable product records.

---

## 2. TLP v2 becomes the new production direction

New production tracker traffic is to move to reviewed TLP v2 protected
application traffic.

TLP v1 remains a **frozen legacy compatibility format**:

- existing v1 byte definitions/golden fixtures are not weakened or rewritten;
- no new product feature is added to v1;
- a new production tracker is not required to emit v1 POSITION after the
  explicit v2 cutover;
- a Gateway may retain a legacy v1 receive path only if mixed-fleet support is
  actually required;
- no protected operation may fall back to plaintext v1.

This document is the explicit product migration direction required before later
implementation slices may stop treating v1 POSITION as the normal production
tracker path.

This does **not** itself allocate new v2 numeric packet/family codes or freeze
wire bytes.

---

## 3. Service ownership remains separate from transport

The new record/envelope design must preserve:

```text
Role
!= Location Source
!= GNSS Power
!= Capability
!= Enabled Service
!= Transport
!= Device Identity
!= Profile
!= User Identity
!= Security Authority
```

And:

```text
Replay
!= Freshness
!= Nonce safety
!= Idempotency
!= Delivery
!= RESULT
```

Storage format is not the wire format.

The durable tracker store owns product observations. TLP v2 owns protected
transport. A future wire-format change must not require reinterpreting the
tracker's durable product facts merely because transport framing changes.

---

## 4. Tracker durable-data policy

### 4.1 Capacity target

Reference RAK4630/RAK4631 tracker profile target:

```text
256 KiB durable product-observation region
= 64 × 4096-byte nRF52840 pages
```

Candidate physical range, subject to the dedicated layout slice and build guard:

```text
0x0A5000 .. 0x0E5000   256 KiB ObservationStore
```

The existing regions above it remain at their current addresses:

```text
0x0E5000 .. 0x0E7000   Geofence
0x0E7000 .. 0x0E9000   Security
0x0E9000 .. 0x0EB000   Config
0x0EB000 .. 0x0ED000   BLE/InternalFS
0x0ED000 .. 0x0F4000   legacy History reservation
0x0F4000 .. 0x100000   bootloader/settings boundary classes
```

The candidate 256 KiB range is not authorized for production writes until:

1. the application ceiling/build guard is deliberately revised;
2. current firmware size/headroom is re-measured;
3. the physical flash backend and ownership guard are reviewed;
4. destructive physical qualification is completed on a development unit.

### 4.2 One physical owner

The 256 KiB region has exactly one owner in one concrete product profile.

For an animal tracker it is the future ObservationStore.

For a Gateway-only custody product/profile the same physical budget may be used
by CustodyStore.

Independent ObservationStore and CustodyStore instances must never both believe
they own the same pages. A future combined tracker+gateway product must receive
an explicit ownership/layout review rather than a hidden Role-based split or
generic allocator.

---

## 5. Periodic observation cadence

Normal durable-record cadence follows the configured report/tracking period.

Examples:

```text
3 min report  -> one 3-minute PERIODIC_OBSERVATION
15 min report -> one 15-minute PERIODIC_OBSERVATION
1 h report    -> one 1-hour PERIODIC_OBSERVATION
```

The activity summary covers that same report period.

Internally, activity classification/feature extraction may use much shorter
windows, but those internal windows are not independently persisted merely
because they exist.

The implementation seed currently using a 5-second/10 Hz ActivityWindow is not a
validated livestock classifier contract and must not be mistaken for the
report-period durable format.

---

## 6. Battery / telemetry policy

Battery measurement must not create an independent wake solely for routine
checking.

Rule:

> Battery is measured when the device is already awake for an existing product
> reason.

A normal battery value is included in the next normal periodic observation. It
does not create an independent flash record for every measurement.

A meaningful battery state transition may create an EVENT after the reviewed
threshold/confirmation policy says that the event is real.

Exact LOW/CRITICAL thresholds remain a hardware/battery physical-validation
decision and are not frozen here.

---

## 7. EVENT policy

An EVENT is a stable occurrence, not a telemetry flag and not a notification
reminder.

Examples include:

- confirmed geofence OUTSIDE;
- LOST;
- reviewed low/critical battery condition;
- persistent sensor/subsystem fault;
- tamper or future safety occurrence.

An occurrence is durably recorded when it becomes real. The same active
condition is not rewritten on every wake.

Lifecycle is conceptually:

```text
ACTIVE -> CLEARED
```

Application/backend owns repeated user reminders, escalation intervals,
notification suppression and acknowledgement UX.

The tracker does not generate repeated durable reminder events merely to make a
phone notify again.

---

## 8. COMMAND_RESULT policy

COMMAND and RESULT remain distinct from delivery.

```text
TX_DONE
!= Gateway receipt
!= command APPLIED
```

For a valid command, the tracker must retain the minimum reviewed durable state
needed for replay safety/idempotency and must durably own the application RESULT
before depending on RF delivery of that RESULT.

A duplicate command must not repeat a side effect merely because the previous
RESULT was lost.

Config mutation continues to obey the separately reviewed ConfigStore state-token
/CAS contract.

Deleting/reclaiming an old RESULT record must not erase the independent replay
or idempotency state that prevents an old command becoming valid again.

COMMAND/RESULT plaintext bytes remain a later wire-format decision; this
document does not override the delegated-command security contract.

---

## 9. Health / diagnostics policy

Health is bounded product state, not an unbounded log.

Use three classes:

1. product-affecting persistent fault/state transition -> durable EVENT;
2. repeated transient/recoverable issue -> bounded counter/status;
3. raw engineering/debug detail -> RAM/USB/development logging, not routine
   product persistence.

Examples of product-affecting faults may include a persistent GNSS/sensor/radio/
storage/security subsystem failure that changes what the product can reliably
do.

Expected/recoverable acquisition retries, transient busy results and normal
development debug prints must not fill product flash one record at a time.

---

## 10. Tracker full-store behavior

Tracker durable storage is bounded. It cannot promise infinite retention.

When the 256 KiB tracker store is full:

1. reclaim already-releasable/obsolete storage where the final storage design
   defines such state;
2. if the retained tracker history itself exceeds finite capacity, reclaim in
   oldest-first order so new product observations continue to be recorded;
3. increment bounded capacity-loss diagnostics and retain enough state for the
   application/backend to distinguish a storage gap from "no observation
   occurred".

Do not silently present an overwritten historical interval as complete.

This tracker policy is intentionally different from Gateway custody.

---

## 11. Gateway custody full-store behavior

An authorized Gateway that already accepted custody of an exact protected object
must not evict that object merely because newer traffic arrives.

For Gateway custody:

- durable commit precedes custody ACK eligibility;
- full/fault/busy/commit-unknown => no custody ACK;
- once custody has been ACKed, the object remains Gateway responsibility until
  Edge durable acceptance permits reclaim;
- admission may close before physical exhaustion to preserve reviewed
  maintenance/emergency headroom;
- the exact watermark/reserve percentages remain an implementation/load-test
  decision, not a value frozen here.

The Gateway uses one custody store, not a fixed 128+128 KiB physical split just
to reserve priority classes.

Priority may control admission/transmission scheduling; it must not retroactively
delete accepted custody.

---

## 12. Gateway -> Edge drain

Initial Gateway -> Edge backlog drain is global oldest-first/FIFO.

For each object:

```text
Gateway HELD
  -> send to Edge
  -> Edge durable accept
  -> Gateway may mark/reclaim
```

A disconnect during transfer leaves the object held at the Gateway. A retry may
produce a duplicate at Edge; idempotency/deduplication must make duplicates safe.

Do not add a complex scheduler before measured need.

A future separate fast path for a live critical event is allowed only if it does
not weaken durable FIFO responsibility or delete accepted backlog.

---

## 13. Multi-gateway reception

Gateways remain independent.

The same tracker protected object may be durably accepted by more than one
authorized Gateway.

The tracker may release its RF responsibility after the first valid,
authorized, exact-object-bound durable custody ACK.

Gateways do not acquire a distributed lock or require another Gateway's
permission to accept a tracker object.

Downstream deduplication is expected. Equivalent authenticated logical content
for the same stable observation identity is idempotent; conflicting
authenticated content for the same logical identity is an integrity conflict.

---

## 14. v2 security constraints

Do not invent cryptography.

New product observation/event/result protection must reuse the reviewed M7P6
security direction unless a focused security review explicitly changes it.

Relevant existing invariants include:

- one independent tracker root credential per credential lifetime;
- gateways do not receive tracker `K_root`;
- D2A/A2D nonce/counter ownership remains with the actual cryptographic sender;
- unauthenticated input never mutates durable replay/application state;
- replay, freshness, nonce safety, idempotency and delivery remain separate;
- gateway custody is opaque by default;
- protected command authority is separate from custody authority;
- delayed immutable store-forward observations require the reviewed old-epoch
  decrypt/dedupe/retirement semantics rather than ordinary command replay rules.

Exact new application-family numeric allocations, plaintext layouts, tag/header
composition and custody-ACK bytes require their own focused wire/security slice.

---

## 15. Offline phone / Edge decryption direction

Gateway remains opaque.

The owner-approved product direction is:

```text
Security Authority / Backend
  -> bounded, scoped offline read authority
  -> authorized Phone / Edge
  -> local decryption of eligible tracker data while Internet is absent
```

Do not copy tracker `K_root` to a phone as a shortcut.

The exact grant/key lifecycle, scope, expiry, revocation and lost-phone blast
radius remain a future reviewed security slice.

This is separate from Gateway custody and separate from BLE bonding.

---

## 16. Legacy 28 KiB History cutover

The legacy HistoryStore at `0x0ED000..0x0F4000` is POSITION-specific and has
physically qualified behavior that must not be casually reinterpreted.

The owner-approved product direction is a **direct v2 cutover**, not permanent
dual-write:

```text
legacy production:
POSITION -> 28 KiB HistoryStore

new production after explicit cutover:
PERIODIC / EVENT / RESULT -> 256 KiB ObservationStore -> TLP v2
```

Do not keep writing the same location to both stores in normal production.

There is no deployed-fleet requirement in this decision to migrate old
development History backlog into the new ObservationStore.

The legacy 28 KiB region is not automatically appended to the 256 KiB journal:
it is non-contiguous because Geofence/Security/Config/BLE own the intervening
pages. Creating a split journal only for that extra capacity is not justified.

Its later reuse/erase/new owner must be a separate explicit layout decision.
Until then, no new writer should claim it merely because the v2 cutover stops
using legacy History for normal production observations.

TLP v1 compatibility fixtures remain intact even though the new production
tracker path no longer depends on v1 POSITION.

---

## 17. ObservationStore format direction

Prefer the smallest fixed-slot, bounded-recovery format that satisfies the
actual product fields.

Initial design target:

- one fixed-size durable record slot;
- record type = PERIODIC / EVENT / COMMAND_RESULT;
- stable record identity;
- observation/event/result time semantics;
- type-specific bounded payload;
- CRC plus commit-last power-cut seal;
- deterministic recovery;
- bounded boot scan and RAM state;
- oldest-first rotation;
- explicit capacity-loss diagnostics.

A 96-byte slot is a **planning target only**, not a requirement. If the reviewed
field set needs 112/128 bytes, preserve product semantics rather than dropping
fields to satisfy an arbitrary target.

Variable-length allocation/fragmentation is not introduced unless fixed slots
prove materially wasteful or insufficient.

Exact page header/slot bytes are intentionally not frozen by this direction.

---

## 18. RF / capacity implications

Capacity must be recomputed from the final v2 protected-frame sizes and real
production cadences.

The design must evaluate at least:

- 3 / 15 / 30 minute report periods;
- 1 / 2 / 4 / 6 / 12 / 24 hour report periods;
- 5 / 10 / 30 / 50 Gateway-source devices;
- routine traffic plus event bursts, retries and duplicates;
- tracker retention time;
- Gateway custody residence time;
- RF airtime/duty-cycle/collision-domain load;
- backlog drain rate versus new-record production rate.

A storage capacity result is not automatically an RF capacity result.

---

## 19. SF4B runtime prerequisites remain mandatory

SF4B proved a portable CustodyStore persistence foundation but did not authorize
production Gateway custody runtime or a physical partition.

Before real custody ACK is enabled, the carried runtime gates remain mandatory,
including:

- bounded recovery for reclaim-intent slot exhaustion;
- handoff-marker exhaustion / head-of-line liveness;
- begin/recovery behavior with a store-owned pending async flash operation;
- distinct propagation of read/storage faults;
- measured/refined nRF52840 scan/program/erase timing before ACK/rendezvous
  timing is frozen;
- reviewed physical flash ownership and FlashMutationGate/SoftDevice behavior.

This v2 tracker-observation direction does not waive those gates.

---

## 20. Implementation order

Proceed in small, reviewable slices:

1. independent architecture review of this cutover direction;
2. exact PERIODIC / EVENT / RESULT semantic field contract;
3. exact v2 protected wire-family/security-envelope review;
4. ObservationStore fixed-slot format + deterministic host power-cut tests;
5. 256 KiB layout/application-ceiling reservation + build guards;
6. RAK4630 build/size verification;
7. destructive physical ObservationStore qualification on a development unit;
8. tracker runtime cutover from legacy POSITION History to the new store;
9. v2 protected RF runtime + Gateway custody integration only after the relevant
   security/SF4B runtime gates close;
10. end-to-end Tracker -> Gateway -> Edge -> Backend validation.

Do not collapse these into one giant rewrite.

---

## 21. Explicitly not frozen here

This document intentionally does **not** freeze:

- PERIODIC/EVENT/RESULT numeric packet/family values;
- exact plaintext byte layouts;
- exact protected-frame sizes;
- exact ObservationStore slot size;
- activity classifier labels/thresholds/window length;
- battery LOW/CRITICAL thresholds;
- Gateway admission watermark percentages;
- offline-read grant cryptography/lifecycle;
- Gateway -> Edge concrete transport selection;
- physical evidence.

Those values require their focused design/test slices.

