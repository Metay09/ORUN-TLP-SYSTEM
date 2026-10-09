# ORUN Application Surface and Transport Direction

Status: **OWNER-APPROVED ARCHITECTURE DIRECTION — DOCUMENTATION-ONLY.**

Baseline: `main@034d0afdbd7b4e26fd2cd44310486f20a61307d7`.

This document records the product/application boundary for USB, BLE and future
LoRa application traffic. It does not implement M7P7H, freeze new BLE/TLP wire
bytes, authorize protected writes, or change TLP v1.

The governing rule is:

> Product semantics belong to application/domain owners. USB, BLE and LoRa are
> transports/adapters into those owners.

A product operation must not acquire a second implementation merely because it
arrived over another transport.

## 1. Topology and ownership

Direct local access:

```text
PC --------USB-------> local ORUN node
Phone -----BLE-------> nearby ORUN node
```

Remote field access:

```text
Phone / PC / backend
        |
   BLE / USB / IP
        |
        v
  gateway-capable ORUN node
        |
       LoRa
        |
        v
 remote target ORUN node / tracker
```

The same target-side application owner must decide the meaning of the operation.

For example:

```text
USB adapter ----\
BLE adapter -----+--> SET_CONFIG --> ConfigService/ConfigStore owner
LoRa adapter ----/
```

The framing, delivery timing and security envelope may differ. The application
meaning must not.

Gateway bridge capability, relay forwarding, tracking, GNSS presence, legacy
Role and security authority remain independent concepts. A gateway is not the
owner of tracker configuration or geofence semantics. A command-capable enrolled
gateway may later be a delegated security issuer; that does not make it the
application owner.

## 2. Transport semantics

### USB

Typical direction:

```text
PC -> local node:
  query / settings / local service command / bounded resource transfer

local node -> PC:
  status / RESULT / diagnostics / bounded resource data
```

USB may retain engineering-only commands that are not product API.

### BLE

Typical direction:

```text
phone -> nearby node:
  query / settings / bounded resource transfer

nearby node -> phone:
  status / RESULT / later accepted location/event/resource data
```

BLE application callbacks remain bounded handoff only. Application execution,
flash, radio, clocks and durable mutation remain loop/domain owned.

### LoRa

Target direction:

```text
gateway/relay -> remote target:
  query / settings / protected command / bounded resource transfer

remote target -> gateway/relay:
  position / status / telemetry / event / RESULT / bounded backlog
```

LoRa is not assumed to be an immediate request/response session. A sleepy target
may receive a previously created protected operation later through store-forward.
Therefore local requester/session identity must not become the owner of mutation
state or application idempotency.

M7P7H does not implement the LoRa adapter. LoRa requirements shape the common
application boundary now so the target-side owners do not need to be rewritten
later.

## 3. Product operation matrix

This matrix distinguishes current runtime from product direction.

| Application semantic | USB | BLE | LoRa | Current note |
| --- | --- | --- | --- | --- |
| GET_CONFIG | implemented | implemented | later | Shared ApplicationRequestService/ConfigStore read |
| DEVICE status | M7P7H | M7P7H | later adapter | Read-only typed snapshot |
| TRACKING/GNSS status | M7P7H | M7P7H | later adapter | Read-only typed snapshot |
| GEOFENCE status | M7P7H | M7P7H | later adapter | Summary only; no geometry/token bytes |
| STORAGE status | M7P7H | M7P7H | later adapter | No O(N) history scan |
| GET_LOCATION | later | later | later | Wait for an accepted/last-known Location owner |
| SET_CONFIG | tracking interval only (`APP INTERVAL <s>`) | later | later | One mutation owner (`ConfigMutationOwner`): serialized slot, durable apply, runtime apply, typed RESULT. Local USB is physical-access trust. Caller-supplied CAS precondition, authentication and BLE/LoRa adapters are later slices on the same owner |
| REPLACE/CLEAR_GEOFENCE | later | later | later | One GeofenceStore/runtime owner; protected resource transfer |
| history/backlog export | local future | local future | bounded later | Must not starve live/critical RF traffic |
| POSITION uplink | local inspection only | local inspection later | implemented TLP v1 uplink | Existing TLP v1 behavior remains frozen |
| COMMAND/RESULT | later | later | later TLP v2 | Security/replay/store-forward required |
| engineering probes | USB preferred | normally no | no | Not product semantics |

"Later" does not mean "copy the USB implementation." It means connect that
transport to the same application operation when its transport/security
prerequisites exist.

## 4. Product API vs engineering/service probes

Current USB commands are not automatically product operations.

Product-facing semantics include:

- GET_CONFIG;
- device identity/runtime summary;
- role mode as observable state, not legacy override;
- capability presence/health;
- tracking/GNSS state;
- geofence resource/runtime summary;
- storage health;
- later accepted location;
- later desired-state configuration and geofence mutation;
- later application commands/results.

Engineering/service-only commands may remain USB-only, including:

- volatile legacy `ROLE TRACKER/RELAY/BASE` override;
- raw `ACCEL?` XYZ sample probe;
- `ACTIVITY START`;
- `BLE?` implementation diagnostics;
- crypto stress/probe commands;
- flash qualification probes.

Do not expose an engineering command over BLE/LoRa merely to achieve transport
symmetry.

## 5. Common application seam rules

The existing `ApplicationRequestService` remains the correct trunk, but its
current GET_CONFIG-shaped response must not grow into one flat struct containing
fields for every future service.

Before adding M7P7H families:

1. Keep one bounded, loop-owned, fixed-memory application seam.
2. Preserve requester provenance as local adapter provenance only.
3. Reshape response data as a common response header plus kind-specific bounded
   POD payload.
4. Do not make the application service depend directly on GNSS/Arduino/driver
   headers. Domain owners/composition provide bounded typed snapshots.
5. Unknown request types remain fail-closed/UNSUPPORTED.
6. Existing GET_CONFIG UUIDs and wire bytes remain byte-compatible.

Exact C++ type names and new BLE type values are implementation details of
M7P7H and are not frozen by this architecture document.

## 6. Access context seam

Transport provenance is not authorization.

The application boundary needs one small access-context seam before more
operations are added, so USB, BLE and future LoRa do not each grow separate
operation allow-lists.

The adapter may report channel/security facts such as:

- local USB;
- open BLE link;
- encrypted BLE link;
- later authenticated LoRa issuer/scope.

The central application boundary decides whether the requested operation is
admissible for that context.

This is intentionally **not** the final authorization system. It is the seam
that lets later authentication/authorization be inserted without rewriting
ConfigStore, GeofenceStore, Location, tracking or other application owners.

All M7P7H operations remain bounded read-only local diagnostics. Location,
geometry/token bytes and protected mutation stay outside that slice.

## 7. Read path vs mutation path

A read response slot is not a durable mutation lock.

Read-only operations may stay synchronous and bounded through the existing
single response slot.

Side-effecting operations must later use a domain-owned serialized mutation
lifecycle that survives longer than one local response transaction:

```text
admission
 -> acquire domain mutation slot
 -> authoritative precondition/CAS checks
 -> durable mutation
 -> runtime apply/reconciliation
 -> typed RESULT
```

A long ConfigStore or GeofenceStore mutation must not block unrelated read-only
queries merely because a response slot is occupied.

Before the first production writer, store APIs must provide enough typed outcome
information to distinguish at least:

- accepted/in progress;
- unchanged/already satisfied;
- busy;
- invalid;
- token/precondition failure;
- maintenance/unavailable;
- confirmed failure before ambiguous durable work;
- OUTCOME_UNKNOWN / reconciliation required;
- completed result plus resulting state token where applicable.

The exact enum names are not frozen here.

## 8. Location boundary

M7P7H intentionally does **not** add GET_LOCATION.

Current code has GNSS fixes and PositionFlow consumers, but no final
transport-neutral accepted/last-known Location owner suitable for a product API.

Future Location work must keep distinct:

```text
GNSS receiver fix
!= accepted Location
!= product freshness/age
!= stale last-known Location
!= persisted/history Location
```

The current 5-second GNSS fresh-fix threshold is a bounded acquisition/TX
acceptance rule. It must not be exported as the product definition of "live."

Recovered/persisted location without trustworthy observation time must never be
presented as live.

M7P7I implements only the missing accepted-Location runtime owner: GNSS is the
sole production producer, the owner starts UNKNOWN after reboot, and no new
GET_LOCATION wire/persistence/source-switching path is added in that owner
slice. Its implementation candidate is PR #63; final audit fixes are applied
and owner revalidation is the remaining merge gate. GET_LOCATION is a later
adapter consumer of that owner, not a direct GnssManager query.

Do not add a speculative multi-source Location framework before a real second
source is implemented.

## 9. M7P7H read-only status families

The first implementation slice after this document is M7P7H.

It exposes four bounded families through the same typed snapshots to USB and BLE:

### DEVICE

Candidate content from existing owners:

- firmware/application-surface revision;
- uptime;
- reset/watchdog reason;
- legacy role plus AUTO/OVERRIDE mode;
- GNSS/accelerometer capability presence/health;
- tracking/relay effective state and reason where already defined.

### TRACKING/GNSS

Candidate content:

- durable requested base tracking interval and provenance;
- currently applied base interval;
- effective runtime interval/cadence mode;
- GNSS state;
- selected bounded GNSS diagnostics.

Requested, applied and effective values must remain separate.

### GEOFENCE

Candidate content:

- durable resource state;
- token state summary, not token bytes;
- area/effective-vertex counts;
- runtime configured state;
- confirmed INSIDE/OUTSIDE/none state;
- confirmation-active state.

A small read accessor may be added to the current coordinator. Do not copy the
full durable geometry merely to answer status.

### STORAGE

Candidate content:

- HistoryStore ready/fail-stop state;
- count/capacity;
- overwritten/corruption/drop counters;
- ConfigStore/GeofenceStore/SecurityStore ready/maintenance summary where
  already available.

Status must not call O(N) history scans such as full `newest()` or
`backlogCount()` traversal.

## 10. Large resources

Do not enlarge every BLE response or add one GATT characteristic per service to
carry large resources.

Large objects such as:

- geofence geometry;
- history export;
- future event/history rings;

use a separately reviewed bounded resource-transfer/staging contract.

The existing M7P7F 20-byte physical frame and 48-byte logical-payload contract
remains valid for the first small status families. A future negotiated transport
revision may optimize bulk transfer if measured need justifies it.

## 11. Compatibility and evidence boundary

This document changes no runtime behavior.

It does not change:

- TLP v1 packet bytes;
- existing BLE service/characteristic UUIDs;
- GET_CONFIG request/response bytes;
- ConfigStore/GeofenceStore/HistoryStore flash formats;
- RF behavior or airtime;
- GNSS power behavior;
- BLE callback/task ownership.

M7P7H implementation must preserve those boundaries and report RAM/flash delta.

This document is not evidence of host PASS, build PASS or physical PASS.
