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

The **current** runtime is not "v1 only": normal live POSITION / relay behavior
still uses the frozen TLP v1 path, while provisioned trackers can also originate
the already-frozen SF3 TLP v2 `HISTORY_SECURE` replay family from the legacy
HistoryStore. SF5 must preserve that evidence boundary until its explicit
runtime cutover.

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

The existing SF2/SF3 `HISTORY_SECURE` wire bytes, vectors and family semantics
are also frozen compatibility evidence. SF5B must define a **new reviewed
product-observation protected family** rather than silently widening or
reinterpreting that 73-byte observation family. At the SF5F direct cutover, the
new family replaces SF3 HISTORY_SECURE as the normal new-record store-forward
path; the legacy SF3 replay runtime is retired together with the legacy
POSITION HistoryStore unless a separately approved migration requirement exists.

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

This separation does not waive the end-to-end custody requirement: if a custody
ACK is bound to one exact protected object, the tracker must retain/reconstruct
whatever transport state is required to validate that exact ACK safely until
responsibility transfers.

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
3. the **DFU bank/update model is explicitly resolved before freezing this
   ceiling**;
4. the physical flash backend and ownership guard are reviewed;
5. destructive physical qualification is completed on a development unit.

The DFU gate is not cosmetic. The candidate application range
`0x026000..0x0A5000` is `0x7F000` = 520,192 bytes. A simple equal two-bank
split of that range would allow about 260,096 bytes per bank, which is smaller
than the current SF4B production image measurement of 285,924 bytes. This does
not prove which DFU model ORUN will use; it proves only that the 256 KiB
reservation and the DFU model cannot be frozen independently.

### 4.2 One physical owner

The 256 KiB region has exactly one owner in one concrete product profile.

For an animal tracker it is the future ObservationStore.

For a device that owns only Gateway custody in the selected product
configuration, the same physical budget may be used by CustodyStore.

ORUN also explicitly allows tracking/sensing and Gateway bridging capability to
coexist on one physical node. That combined case is not a new Role and is not a
speculative corner case. Before SF5D freezes a physical owner/layout, it must
therefore define how one such device can durably own both its **own product
observations** and **foreign opaque custody** without two independent stores
writing the same pages and without silently reducing either product guarantee.

Until that focused ownership decision is made, ObservationStore and CustodyStore
must never both believe they own the same physical pages. Do not select the
owner by legacy Role and do not add a generic allocator merely to avoid making
the explicit product decision.

A profile/capability change must never implicitly reinterpret or format a
non-empty owner region. Changing from observation ownership to custody ownership,
or the reverse, requires an explicit maintenance transition that proves the old
responsibility is closed (for example durable downstream handoff/drain) or an
explicit destructive re-baseline that surfaces accepted data loss. Unresolved
Gateway custody blocks owner transition. Profile selection alone is never an
erase ceremony.

---

## 5. Periodic observation cadence

Normal durable-record cadence follows the **effective report period actually
selected by Tracking policy for that interval**. This may be the configured base
cadence or a separately reviewed adaptive cadence such as the existing geofence
OUTSIDE policy; an old record is never reinterpreted after policy/config changes.

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

The minimal state needed to preserve an **open occurrence identity** is durable
control state, not expendable historical backlog. It must survive reset and must
not disappear merely because the historical EVENT record ages out of the
oldest-first ObservationStore ring. SF5C may choose the concrete owner/bytes,
but one owner must retain enough state to emit a later CLEARED transition for
the same occurrence or to perform an explicit fail-closed reconciliation.

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
needed for replay safety/idempotency and must durably own the **logical command
outcome** before depending on store-forward RF delivery of that RESULT.

Durable RESULT ownership is per logical command in its authenticated authority
context, **not per cryptographic retry**. Repeating the same `command_id` with
the same logical payload/target must not append an unbounded sequence of
persistent RESULT records. While a retained durable RESULT exists, retries
reuse that logical result. For the first desired-state ConfigStore family, the
reviewed CAS state may reconstruct `ALREADY_SATISFIED` after RESULT loss
without another application-effect flash write, consistent with the delegated
command contract. SF5B/SF5C must define a bounded RESULT retention/deduplication
rule rather than creating one durable record for every new security counter.

A duplicate command must not repeat a side effect merely because the previous
RESULT was lost.

Unauthenticated, malformed or replay-rejected command traffic must not be able to
fill the tracker durable-data store with RESULT records. Authentication,
authorization/replay and command admission happen before a durable application
RESULT is created for legitimate traffic. A valid authenticated command may
still produce a durable application rejection RESULT when the application
contract requires one.

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

1. reclaim already-released/obsolete historical storage according to the
   reviewed release-state rules;
2. if the retained tracker history itself exceeds finite capacity, reclaim in
   oldest-first order so new product observations continue to be recorded;
3. increment bounded capacity-loss diagnostics and retain enough state for the
   application/backend to distinguish a storage gap from "no observation
   occurred".

ObservationStore must also own or reference **bounded reset-safe release state**
for responsibility already transferred by authenticated custody ACK. Because
critical/current RF QoS can produce out-of-order acknowledgements, a single
contiguous watermark is not automatically sufficient. SF5C must either represent
bounded selective/non-prefix release safely or constrain scheduling so the
durable representation remains correct.

A custody ACK that is only remembered in RAM may cause a safe duplicate after
reset, but duplicate amplification must be bounded. SF5B/SF5C must quantify and
test the maximum replay/re-protection amplification per retained record and per
reset/checkpoint cycle; reset must not make the complete backlog look
indefinitely unreleased.

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

SF5B must also close the **retransmission identity** question created by opaque
custody. A Gateway may acknowledge only an exact durably committed protected
object, while a tracker ObservationStore owns the logical product observation.
After retry/reset, the tracker must never reuse an AEAD nonce/counter
unsafely or accidentally turn one logical observation into an unbounded stream
of distinct opaque custody objects. SF5B/SF5C must explicitly define whether
the exact protected object is durably retained, deterministically reconstructable
from separately durable state, or intentionally re-protected under a new
counter with bounded duplicate semantics. Do not assume these are equivalent.

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

A future **read** grant must not silently become tracker-origin authentication
authority. In particular, SF5B must not freeze an envelope/key-binding design
that makes offline decryption possible only by granting the phone/Edge the
ability to forge tracker-originated PERIODIC/EVENT/RESULT traffic. If a proposed
symmetric construction cannot provide that separation, the limitation is an
explicit security decision/blocker before wire freeze, not something hidden in a
later mobile implementation.

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
current runtime:
live POSITION / relay             -> frozen TLP v1 path
28 KiB HistoryStore backlog       -> frozen SF3 TLP v2 HISTORY_SECURE replay
                                    (when provisioned)

new production after explicit SF5F cutover:
PERIODIC / EVENT / RESULT         -> 256 KiB ObservationStore
                                  -> new reviewed SF5 TLP v2 protected family
```

SF5 does not reinterpret the frozen SF2 HISTORY_SECURE bytes. The new SF5 family
replaces HISTORY_SECURE for **new product observations** at cutover. With no
deployed-fleet migration requirement, the legacy development History backlog is
not automatically migrated; any erase/re-baseline at cutover must be explicit
and must not be reported as preserved history.

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
- oldest-first historical rotation;
- explicit capacity-loss diagnostics;
- bounded reset-safe tracker responsibility-release state, including safe
  treatment of out-of-order custody facts;
- durable active-EVENT occurrence state that is not lost merely because a
  historical record rotates out.

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

The current SF4B store format is intentionally bound to the existing **73-byte
HISTORY_SECURE observation object** (92-byte on-flash record, 43 records/page).
SF5 product observations/events/results may produce a different protected-object
size. Therefore SF5G must not silently reuse the current 73-byte geometry or its
capacity arithmetic. Any widening/versioning of CustodyStore is a new reviewed
format decision with fresh power-cut, recovery, wear, scan and capacity analysis.

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
3. exact v2 protected wire-family/security-envelope review, including
   HISTORY_SECURE replacement/retirement, retry/reset protected-object lifetime,
   bounded logical-RESULT retry behavior, and offline-read-vs-forgery separation;
4. ObservationStore fixed-slot format + deterministic host power-cut tests,
   including reset-safe selective release state and active-EVENT lifecycle state;
5. 256 KiB layout/application-ceiling reservation + build guards, only after
   the DFU bank/update model and owner-transition ceremony are resolved;
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

