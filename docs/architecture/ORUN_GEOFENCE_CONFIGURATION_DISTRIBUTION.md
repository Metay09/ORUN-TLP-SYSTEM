# ORUN Geofence Configuration and Distribution Direction

Status: **OWNER-APPROVED DESIGN DIRECTION; DOCUMENTATION-ONLY — 2026-09-27**

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

- service enabled/disabled state as explicit semantics;
- zero or more permitted polygons subject to the bounded production capacity;
- each polygon's ordered E7 latitude/longitude vertices;
- future metadata only when a real product requirement exists.

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

A future measured airtime problem may justify reviewed secure group distribution
or content caching. Do not add that complexity before real fleet/RF-domain
measurements require it.

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
OFFLINE / UNREACHABLE
```

Exact public naming may be refined later, but the distinction is mandatory.

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

Conceptually:

```text
BLE frame(s) ----> adapter --\
                              \
                               > Geofence config mutation service
                              /
secure LoRa -----> adapter --/
```

The common owner must perform, in one serialized transaction:

1. application authorization;
2. precondition/CAS admission;
3. complete candidate validation;
4. durable staging/commit;
5. active-snapshot publication;
6. resulting state/result capture.

A second mutation arriving over another transport while one is authoritative
must receive bounded BUSY/retry behavior. Two transports must not each
independently accept the same stale precondition.

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

Required semantic properties:

- stable transaction identity;
- resource/version identity;
- bounded total length;
- bounded fragment count;
- fragment index/offset;
- duplicate-safe fragment acceptance;
- complete-resource integrity check before activation;
- timeout/abandon behavior;
- idempotent retry;
- no partial geometry publication.

Exact BLE and LoRa wire framing remains unfrozen.

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
- interaction with bootloader/application ceiling.

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

### LoRa/remote

Remote change may originate from:

```text
app -> backend -> enrolled gateway -> LoRa -> tracker
```

or from an authorized offline gateway path under the reviewed delegated-authority
model.

The gateway may queue/store-forward the protected mutation for a sleepy tracker.

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

Initial safe rule:

- fan out per device;
- pace by RF-domain airtime budget;
- coalesce superseded pending desired state;
- prioritize critical event traffic over bulk configuration rollout;
- do not retransmit already-applied snapshots;
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

## 15. Implementation gates

Before production geofence configuration is enabled, separately close:

1. bounded total polygon/vertex/snapshot capacity from RAM/flash/airtime math;
2. persistence owner + exact partition/layout + power-cut recovery;
3. CAS token scope for the geofence resource;
4. application authorization for BLE local writes;
5. protected TLP v2 mutation path for LoRa;
6. bounded fragmentation/reassembly/integrity;
7. explicit `REPLACE_SET` / `CLEAR_SET` semantics;
8. per-device result/reconciliation model;
9. group/fleet fan-out load simulation for ~10, ~30-50 and ~100 devices;
10. host fault tests for duplicate/missing/out-of-order fragments, reset and
    outcome-unknown;
11. RAK4630 build/RAM/flash/ownership guards;
12. focused physical BLE + LoRa transfer/reboot tests when those runtime paths
    exist;
13. only then wire the durable active set into M6D operational-state runtime.

The important ordering is:

```text
configuration resource contract
-> capacity/storage/CAS
-> secure BLE/LoRa mutation path
-> atomic active geofence set
-> M6D runtime state/cadence integration
-> secure OUTSIDE EVENT delivery
-> backend/mobile product UI
```

Do not build a map UI that cannot be safely committed to the device, and do not
wire M6D to a fake hard-coded production fence merely to demonstrate the state
machine.
