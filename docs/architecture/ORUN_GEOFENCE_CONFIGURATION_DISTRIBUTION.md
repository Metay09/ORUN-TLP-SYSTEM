# ORUN Geofence Configuration and Distribution Direction

Status: **OWNER-APPROVED DESIGN DIRECTION; INDEPENDENT FINAL VERIFY PASS WITH MINOR DOC FIX; 0 BLOCKER / 0 HIGH / 0 MEDIUM; F-L1–F-L4 DOC FIXES APPLIED — 2026-09-27**

This document defines how geofence configuration must eventually be created,
transported, applied, removed and distributed without tying configuration
semantics to BLE, LoRa, one gateway, one device role or one UI.

It is intentionally pre-wire and pre-storage-allocation. It does **not** freeze
TLP v2 COMMAND/RESULT bytes, BLE characteristic payload bytes, a GeofenceStore
flash partition, backend schema or mobile UI implementation.

The current geometry/runtime sources remain:

- `firmware/include/geofence_geometry.h`;
- `firmware/include/geofence_area_set.h`;
- `docs/architecture/ORUN_GEOFENCE_OPERATIONAL_POLICY.md`.

The existing config CAS/security directions remain authoritative for protected
mutation semantics.

---

## 1. Product behavior

The user must be able to:

- draw/edit permitted livestock areas on a map;
- apply a geofence configuration to one tracker;
- apply the same logical configuration to a selected group/herd;
- apply it to all authorized trackers in the user's fleet;
- remove/clear that geofence configuration at the same scopes;
- perform a device mutation locally through BLE when appropriate;
- perform the same semantic mutation remotely through an authorized
  gateway/LoRa path;
- see per-device application state rather than treating "sent" as "applied".

Transport is not configuration ownership:

```text
BLE != LoRa != Config owner

BLE adapter ----\
                 > one geofence/config mutation owner
LoRa adapter ---/
```

A BLE-originated and a LoRa-originated request that express the same desired
state must converge on the same validation, authorization, CAS and durable
commit logic.

---

## 2. Geometry capacity: do not invent a six-point product limit

The current validated geometry core already supports:

```text
3..64 effective vertices per polygon
```

An optional explicit closing vertex equal to the first does not consume an
additional effective-vertex slot.

Therefore the product/UI must not introduce an arbitrary six-point limit.

A tracker may also have multiple simultaneously permitted polygons. The current
area-set layer deliberately does not freeze a product limit on simultaneous
area count.

Before production configuration integration, ORUN must derive a bounded total
area/vertex budget from:

- RAM;
- durable flash allocation;
- A/B or equivalent atomic-update overhead;
- transfer fragmentation;
- LoRa airtime and duty-cycle;
- validation CPU time;
- ~10 / ~30-50 / ~100 device distribution load.

The resulting limit must be a measured implementation capacity, not an
arbitrary UX number. Raising the existing 64-effective-vertex-per-polygon
geometry bound requires a separate arithmetic/validation review; this document
does not silently change that proven bound.

---

## 3. Logical geofence resource

The device-facing semantic resource is one **active permitted-area set**.

A desired snapshot contains:

- service enabled/disabled state as explicit semantics **owned by this same
  geofence resource**; do not split the enabled bit into ConfigStore while the
  polygons live under another persistence owner;
- when enabled, **one or more** permitted polygons subject to the bounded
  production capacity;
- each polygon's ordered E7 latitude/longitude vertices;
- future metadata only when a real product requirement exists.

`enabled + zero polygons` is invalid and must be rejected. `CLEAR_SET`
means "no configured active geofence / geofence service disabled"; it is not an
empty-but-enabled set.

The first mutation model should prefer whole-resource operations:

```text
REPLACE_SET(desired complete set)
CLEAR_SET
```

rather than exposing many independent flash-mutating "add vertex", "delete
vertex" or "delete polygon" commands.

The app may let the user edit one polygon, but it can calculate the resulting
complete desired set and submit one atomic replacement. This keeps recovery,
CAS and idempotency tractable.

`CLEAR_SET` is an explicit authorized mutation. Do not use invalid geometry,
`0,0`, an empty packet, erased bytes or transport timeout as an implicit delete
sentinel.

`CLEAR_SET` is also **not FREE_GRAZE**. FREE_GRAZE is an operational policy
that may suppress normal geofence-violation alarm behavior while tracking
continues; CLEAR_SET removes/disables the configured geofence resource itself.

Every REPLACE_SET must validate the complete set using all current M6C rules,
not vertex count alone:

- 3..64 effective vertices per polygon;
- valid E7 coordinate domain;
- current 10-degree local span bound;
- no duplicate adjacent vertices;
- non-degenerate geometry;
- no self-intersection;
- current pole/antimeridian singularities remain unsupported;
- if **any** polygon is invalid, reject the complete replacement.

Multiple polygons retain the M6C2 **permitted union** semantics: inside any
valid polygon is permitted, overlapping polygons are allowed, polygon ordering
has no product meaning, and there is currently no hole/exclusion-zone semantic.
Any reported `area_index` is diagnostic, not area priority.

---

## 4. Device, group and fleet targeting

Target selection belongs to user/backend/gateway orchestration, not to the
tracker's geofence geometry owner.

User-visible scopes are:

```text
DEVICE
GROUP / HERD
FLEET / ALL AUTHORIZED TRACKERS
```

For the initial production design, GROUP/FLEET means:

```text
one logical user operation
        |
        v
authorized target resolution
        |
        v
per-device protected mutation fan-out
        |
        v
per-device RESULT / reconciliation
```

A tracker should receive a mutation addressed/authorized for that tracker. Do
not make a fleet-wide shared command secret or an unauthenticated "all trackers
apply this" LoRa broadcast the initial authority model.

Per-device protected fan-out remains the authority model, but the airtime
analysis in §12 already shows that naïvely repeating a large identical geometry
blob per tracker can be operationally expensive. Therefore implementation must
evaluate content-addressed reuse/caching or another reviewed secure distribution
optimization **before** large fleet rollout if the measured snapshot size and RF
profile make direct fan-out impractical. This does not authorize group
authentication keys, unauthenticated multicast or a shared fleet command secret.

Group membership is backend/application domain state. It must not be encoded as
`NodeRole`, radio capability, location source or device identity.

---

## 5. Per-device result truth

A bulk operation is not complete merely because the backend/gateway transmitted
it.

The orchestration layer must retain per-device state such as:

```text
PENDING
DELIVERED_TO_GATEWAY
DELIVERED_TO_DEVICE
APPLIED
ALREADY_SATISFIED
STALE_PRECONDITION
STATE_UNCERTAIN
REJECTED
FAILED
UNCONFIRMED
OFFLINE / UNREACHABLE
```

Exact public naming may be refined later, but the distinctions are mandatory.

Result truth rules:

- `DELIVERED_TO_GATEWAY` means only gateway custody;
- `DELIVERED_TO_DEVICE` may be used only when there is authenticated evidence
  that the target device accepted/received the relevant protected object; RF
  TX completion is insufficient;
- `APPLIED`, `ALREADY_SATISFIED`, `STALE_PRECONDITION`,
  `STATE_UNCERTAIN`, `REJECTED` and definitive `FAILED` require the
  corresponding authenticated application RESULT/reconciliation evidence;
- a bounded transport/custody timeout without authenticated target RESULT is
  `UNCONFIRMED`, never `FAILED`;
- `OFFLINE / UNREACHABLE` is an orchestration observation that no delivery
  opportunity is currently available; it must not erase a previously
  `UNCONFIRMED` outcome after a protected attempt may have reached the device.

For example:

```text
Fleet operation: "replace pasture A"

97 APPLIED
2 PENDING/OFFLINE
1 STALE_PRECONDITION
```

must not be shown as "100 devices updated".

`TX_DONE`, BLE write submission or gateway custody is not device application
success.

Removal follows the same rule. An offline tracker remains configured until it
actually applies the authenticated CLEAR/REPLACE mutation.

---

## 6. One mutation owner across BLE and LoRa

The existing config CAS direction requires one serialized semantic config owner
across all transports. Geofence configuration must preserve that rule.

Transport adapters may perform framing/reassembly, but must not own the durable
truth.

Long-running fragment **staging is not the serialized semantic mutation
transaction**. A remote transfer may take minutes or hours and must not hold the
single mutation/CAS slot for that duration or block a nearby recovery/config
operation over BLE.

Conceptually:

```text
BLE frame(s) ----> adapter --\
                              \
                               > Geofence config mutation service
                              /
secure LoRa -----> adapter --/
```

The flow is split deliberately:

**Transfer/staging phase**

- authenticate/authorize each protected transfer action as required by its
  transport/security contract;
- accept only bounded, transaction-identified candidate fragments;
- stage them under a non-authoritative candidate identity;
- allow duplicate-safe resume/restart/abandon;
- do not evaluate authoritative desired-state equality or consume the mutation
  CAS slot merely because the first fragment arrived.

**Short semantic commit phase**, entered only after the complete candidate and
its content integrity are verified:

1. acquire the single geofence semantic mutation slot;
2. perform authoritative application authorization/precondition/CAS admission;
3. validate the complete candidate geometry/resource;
4. durably commit and atomically switch authority;
5. publish the active snapshot;
6. capture resulting state/result fields;
7. release the mutation slot.

A second semantic mutation arriving over another transport while this short
commit is authoritative must receive bounded BUSY/retry behavior. Two
transports must not each independently accept the same stale precondition.

A locally authorized BLE operator **may abandon an incomplete remote staging
candidate** before semantic commit, provided the future storage contract makes
that cancellation explicit, bounded and power-cut safe. Cancellation of
non-authoritative staging is not permission to bypass authorization or CAS for
the replacement that follows. Once the semantic commit phase has acquired the
mutation slot, no transport may preempt it.

BLE bonding alone is not ORUN application authorization.

LoRa remote mutation must not use unauthenticated TLP v1. It belongs on the
reviewed protected TLP v2 command/security path.

---

## 7. CAS/token boundary

The existing 96-bit ConfigStore application state token is tied to the current
small ConfigStore semantic state.

A future large geofence snapshot must **not silently reuse that token as if it
already covered geofence bytes**.

Before implementation, choose and independently review one of these coherent
models:

1. a separate opaque geofence-resource CAS token with the same
   VALID/UNAVAILABLE/UNCERTAIN semantics; or
2. an explicitly designed composite config transaction/token that atomically
   covers all participating persistent owners.

Do not fake atomicity across ConfigStore and a future GeofenceStore.

Whichever model is selected must preserve:

- stale-write rejection;
- A -> B -> A token non-reuse;
- reset-safe idempotency;
- serialized mutation ownership;
- `UNCONFIRMED / OUTCOME_UNKNOWN` when durable outcome cannot yet be proven.

If the future design chooses a **composite token/transaction** spanning
ConfigStore and geofence persistence, the "single geofence semantic mutation
slot" in §6 becomes part of the shared composite config-mutation authority. It
must not remain an independent lock that could allow ConfigStore and geofence
owners to accept conflicting halves of one composite precondition.

The exact token scope is therefore an implementation-design gate, not frozen by
this direction document.

---

## 8. Large snapshot transfer

A geofence can be much larger than the current small configuration record.

One E7 vertex is already 8 raw bytes before framing/metadata:

```text
latitude_e7  int32
longitude_e7 int32
```

A 64-vertex polygon is therefore 512 raw coordinate bytes before polygon,
transaction, authentication and integrity metadata. Multiple polygons increase
that further.

Consequences:

- do not put the geofence blob into TLP v1;
- do not assume one BLE ATT frame;
- do not assume one LoRa packet;
- do not expand the current 48-byte ConfigStore v2 record into a generic blob
  store.

The transfer layer must support a bounded multi-fragment logical mutation.

The existing delegated desired-state COMMAND candidate is **not** a suitable
large-resource transport. Its current 32-byte protected plaintext ceiling has a
24-byte fixed COMMAND portion, leaving only 8 bytes for ordinary config args.
A raw 64-vertex polygon alone is 512 coordinate bytes, before polygon,
transaction, digest and integrity metadata. Geofence transfer therefore needs a
separately reviewed protected **resource-transfer** contract; do not encode it
as dozens of ordinary small-config COMMAND mutations.

That resource-transfer design must explicitly reconcile with the delegated
security rule of one distinct outstanding protected frame per tracker and with
durable sender-counter/replay-HWM ownership. Fragment counters, retries and
RESULT cadence must be reviewed as one security/airtime design rather than
invented inside the geofence service.

Before sending a large body over LoRa, the protocol must provide a small
authenticated resource-state/precondition step that can compare at least the
target resource identity/CAS state and content digest (or an equivalently
reviewed content identity). Its purpose is to detect
`ALREADY_SATISFIED`, `STALE_PRECONDITION` or `STATE_UNCERTAIN` **before**
re-sending a large body, especially after RESULT loss/retry. This query does not
make the body authoritative and does not replace final commit-time CAS.

Required semantic properties:

- stable transaction identity;
- resource/version identity;
- bounded total length;
- bounded fragment count;
- fragment index/offset;
- duplicate-safe fragment acceptance;
- complete-resource content digest/integrity check before commit;
- authenticated precondition/content-identity query before large LoRa transfer;
- timeout/abandon behavior;
- idempotent retry;
- no partial geometry publication.

Exact BLE and LoRa wire framing remains unfrozen.

Because of the payload asymmetry, **BLE is the preferred bulk-transfer path when
the operator can reach the tracker locally**. LoRa remains required for remote
configuration, but large remote rollouts must be airtime-budgeted and may need a
reviewed content-reuse/caching design before fleet-scale use.

---

## 9. Atomic activation and failure behavior

The currently active geofence remains authoritative while a replacement is
being transferred or staged.

Required behavior:

```text
old ACTIVE set
    |
receive/stage candidate fragments
    |
validate complete snapshot + integrity + geometry
    |
durably commit candidate
    |
atomic authority switch
    |
new ACTIVE set
```

If phone connection, RF, power or reset interrupts before commit:

- the old active set remains usable;
- an incomplete candidate must not become active;
- reboot recovery must never assemble authority from a partial set;
- retry may resume or restart according to the future storage contract;
- unknown durable outcome must reconcile before a definitive RESULT.

This is especially important for fleet changes: a tracker must never spend hours
with "half the new pasture polygon" active.

### 9.1 Effect on M6D operational state

A geofence snapshot/version change invalidates INSIDE/OUTSIDE evidence collected
against the previous geometry.

On successful `REPLACE_SET` commit:

- cancel any in-progress transition-confirmation episode from the old set;
- do not synthesize an INSIDE/OUTSIDE transition event merely because config
  changed;
- treat the operational classification as internally **unclassified/pending
  fresh evidence** (this is not a third user-visible geofence state);
- use base cadence `B` while unclassified;
- evaluate the next accepted fresh Location against the new active set;
- if that evidence indicates OUTSIDE, the normal bounded confirmation policy
  still applies before entering confirmed OUTSIDE and switching to `B / 3`.

On successful `CLEAR_SET`:

- disable geofence evaluation;
- cancel any in-progress confirmation episode;
- clear prior geofence operational authority tied to the removed set;
- restore effective tracking cadence to configured base `B`;
- emit no synthetic INSIDE/return event solely because the fence was cleared.

FREE_GRAZE remains a separate operational policy and is not implied by either
REPLACE_SET or CLEAR_SET.

---

## 10. Persistence ownership

The current ConfigStore v2 two-page record is optimized for small durable
requested configuration and token state.

Large/multi-polygon geofence bytes have different capacity, update and transfer
properties. Do not casually consume ConfigStore, HistoryStore, SecurityStore,
BLE/InternalFS or nearby "apparently free" flash.

Before implementation, a dedicated geofence-persistence slice must define:

- explicit owner;
- exact flash range/partition source;
- snapshot format and forward-compatibility discriminator;
- active/staging recovery invariant;
- atomic commit mechanism;
- reset and factory-reset semantics;
- wear budget;
- corruption handling;
- unsupported-newer behavior;
- maximum total snapshot bytes;
- interaction with bootloader/application ceiling;
- interaction with the bootloader/DFU dual-bank budget so a new geofence
  partition does not silently reduce firmware-update safety margin below the
  accepted product requirement.

A dedicated owner such as a future `GeofenceStore` is the likely shape, but
this document does not allocate pages or require that class name.

---

## 11. BLE versus remote LoRa behavior

### BLE/local

A nearby authorized client may transfer the same desired geofence resource over
BLE.

BLE is primarily a **per-device transport**. A phone may sequentially configure
multiple nearby devices, but "group/fleet" is not a magic BLE broadcast
authority.

Local BLE must remain usable without live Internet when the authorization model
allows it.

The tracker still does not own a user/phone ACL. BLE application authorization
therefore requires a separately reviewed ORUN authority mechanism above BLE
bonding—for example a backend-issued offline grant plus proof-of-possession, or
another explicitly reviewed local-owner credential model. This document does
not choose/freeze that mechanism; it only forbids treating bond/PIN state as
authorization.

### LoRa/remote

Remote change may originate from:

```text
app -> backend -> enrolled gateway -> LoRa -> tracker
```

or from an authorized offline gateway path under the reviewed delegated-authority
model.

The gateway may queue/store-forward a protected geofence mutation for a sleepy
tracker **only after** the delegated-command contract explicitly admits the
geofence resource-transfer family, its opcode/scope binding and its freshness/
precondition semantics. The current initial allowed family in delegated §12.1
does not, by itself, authorize this large geofence resource transfer.

The tracker remains the final authority for:

- authentication result;
- replay acceptance;
- CAS/precondition;
- candidate validation;
- durable commit;
- resulting APPLIED/REJECTED state.

---

## 12. Fleet distribution and RF airtime

A 10-device solution must not become an airtime collapse at 100 devices.

The current reference RF candidate (SF11 / BW125 / CR4/5) gives approximately
**2.134 s airtime for one 96-byte LoRa frame** under the ordinary explicit-header
LoRa airtime formula. This is an engineering illustration, not a regulatory
duty-cycle hardcode.

The current delegated small-config COMMAND candidate allows 32 protected
plaintext bytes with 24 bytes already consumed by fixed COMMAND fields. If one
naïvely tried to carry a 512-byte/64-vertex raw polygon through that family, the
8-byte remaining args budget would imply roughly 64 body-bearing command frames
even before resource metadata. A separately designed resource frame with roughly
32 body bytes would still be about 16 frames. Therefore an illustrative direct,
lossless, one-polygon **gateway TX airtime** range is:

| trackers | 16 frames/device | 64 frames/device |
| ---: | ---: | ---: |
| 10 | ~0.10 h | ~0.38 h |
| 40 | ~0.38 h | ~1.52 h |
| 100 | ~0.95 h | ~3.79 h |

If an installation were subject to an effective 10% transmit-duty budget, those
figures imply roughly ten times the wall-clock minimum before retries, RESULT
traffic, sleepy receive rendezvous, normal telemetry, alarm priority or relay
duplication. A one-hop relay may add another transmission of the large body in
the same RF domain. These numbers are deliberately conservative warning
arithmetic, not a production throughput promise.

Therefore:

- geofence bulk transfer must not be forced through the ordinary 32-byte
  desired-state COMMAND family;
- the future protected resource-transfer slice must define fragment/counter/
  RESULT cadence with the delegated single-outstanding-frame rule;
- perform the small authenticated precondition/content-digest query before bulk
  LoRa transfer;
- prefer BLE for large local configuration when available;
- fan out authority per device, but pace by RF-domain airtime budget;
- coalesce superseded pending desired state;
- prioritize critical event traffic over bulk configuration rollout;
- do not retransmit a large body when authenticated state/digest reconciliation
  proves it already applied;
- evaluate secure content reuse/caching or another reviewed distribution
  optimization before large identical fleet rollout when measured airtime
  warrants it;
- use authenticated result/reconciliation before declaring completion.

If a user changes a geofence twice while 40 trackers are offline, the
orchestrator should normally retain the newest desired state rather than force
every device to consume both obsolete large snapshots, unless audit/history
semantics require otherwise.

Fleet count is not the only scale variable. Collision-domain size, SF/BW,
fragment count, relay duplication and sleepy receive opportunities determine
real cost.

---

## 13. Map/UI semantics

The map is an authoring/rendering surface, not the device's source of truth.

The phone/backend converts the user's shape to ordered E7 polygon coordinates.
The tracker stores/evaluates coordinates, not map tiles.

The UI must:

- support more than six vertices;
- support multiple permitted polygons subject to device capacity;
- show the device/apply status of a bulk operation;
- distinguish desired backend state from confirmed device-applied state;
- expose conflicts/failures/offline devices;
- require explicit confirmation for destructive fleet/group CLEAR operations.

If the UI simplifies a hand-drawn polygon to fit device capacity, it must show
the resulting geometry before commit. It must not silently move the fence.

---

## 14. Compatibility and non-impact

This direction does not currently change:

- TLP v1 bytes/sizes;
- existing POSITION/store-before-send flow;
- M5 one-hop relay behavior;
- ConfigStore v2 record bytes;
- SecurityStore format;
- HistoryStore format;
- BLE application GATT bytes;
- GNSS power/acquisition ownership;
- role semantics;
- current 64-effective-vertex geometry bound;
- current area-set behavior.

It does not claim that group/fleet configuration, remote geofence mutation,
GeofenceStore persistence, secure EVENT transport, backend map UI or Android
runtime already exists.

---

## 15. Implementation gates and sequencing

M6D2's **local operational runtime is implemented and merged and is not blocked
by M7/TLP v2**. It currently consumes a bounded in-memory area set but the normal
production image deliberately leaves that area set unconfigured. Focused
physical GNSS/geofence validation may continue to use the existing
compile-gated/test-only fixture that is never part of the production
configuration source. The prohibition on a "fake hard-coded production fence"
does **not** prohibit that explicit test fixture.

The durable geofence source itself is now implemented through M6D3B, and
M6D3C supplies its recovered semantic snapshot to M6D2 at boot. What remains
gated on configuration/security work is the **user-mutable remote/local
protected mutation path**. No BLE/LoRa geofence writer is authorized by these
storage/runtime slices. The original foundation contract remains recorded in
`docs/milestones/M6D3.md`, including the production capacity, dedicated
persistence range and independent geofence-resource CAS namespace.

Before production geofence configuration is enabled, separately close:

1. bounded total polygon/vertex/snapshot capacity from RAM/flash/airtime math;
2. persistence owner + exact partition/layout + power-cut recovery, including
   bootloader/application/DFU dual-bank budget;
3. CAS token scope for the geofence resource;
4. application authorization for BLE local writes without a tracker user/phone
   ACL;
5. a separately reviewed protected TLP v2 resource-transfer path for LoRa,
   explicitly reconciled with delegated §7.3 single-outstanding-frame rules;
6. explicit delegated opcode -> scope registry entries for geofence resource
   state/read, REPLACE and CLEAR as appropriate; CLEAR must have authorization
   scope capable of representing its alarm-protection impact rather than being
   silently treated as a harmless read;
7. explicit admission of the geofence resource family to delayed store-forward
   freshness/precondition policy before any gateway queues it;
8. bounded fragmentation/reassembly/content-integrity plus the authenticated
   precondition/content-digest query;
9. explicit `REPLACE_SET` / `CLEAR_SET` semantics, including enabled+empty
   rejection and FREE_GRAZE separation;
10. config-change interaction with M6D state: cancel old confirmation,
    unclassify/re-evaluate on REPLACE, B cadence while unresolved, normal 2-of-3
    for new OUTSIDE evidence, and CLEAR -> B with no synthetic return event;
11. per-device result/reconciliation taxonomy including `UNCONFIRMED`;
12. group/fleet load simulation for ~10, ~30-50 and ~100 devices using actual
    candidate frame sizes, retry, RESULT, relay and sleepy-RX behavior;
13. host fault tests for duplicate/missing/out-of-order fragments, concurrent
    BLE/LoRa staging, staging cancellation, reset and outcome-unknown;
14. RAK4630 build/RAM/flash/ownership guards;
15. focused physical BLE + LoRa transfer/reboot tests when those runtime paths
    exist;
16. Event/backend alarm-lifecycle semantics for authenticated REPLACE/CLEAR while
    an OUTSIDE alarm is already open: config mutation must not synthesize an
    INSIDE event, but the backend must have an explicit authenticated way to
    mark the old alarm as superseded/cleared-by-configuration rather than
    leaving a permanently open alarm.

The development ordering is intentionally two-track:

```text
M6D local behavior:
operational state-machine contract            [DONE]
-> bounded runtime composition + host tests   [DONE]
-> test-only fixture                          [DONE]
-> focused GNSS/geofence physical validation  [DEFERRED]

Production configuration:
resource contract                             [DONE]
-> capacity/storage/CAS                       [DONE: M6D3/M6D3A]
-> atomic durable active geofence source      [DONE: M6D3B]
-> connect production provider to M6D2        [DONE: M6D3C, PHYSICAL PASS]
-> secure BLE + protected LoRa resource transfer [NOT IMPLEMENTED]

Then:
secure OUTSIDE EVENT delivery
-> backend/mobile product UI
```

Do not build a map UI that cannot eventually be safely committed to the device,
but do not delay the local M6D state machine merely because production remote
configuration and TLP v2 resource transfer are not yet implemented.
