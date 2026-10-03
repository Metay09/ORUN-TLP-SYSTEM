# M7P7I — Accepted Location Owner Foundation

Status: **IMPLEMENTATION CANDIDATE — OWNER VALIDATION / AUDIT PENDING; NOT MERGE-READY.**

Planning baseline:
`main@3aa36c67f2f9fe41d183af2729afb5dbf4b94ef5` (M7P7H merged).

M7P7H is merged and closed. Its changed DEVICE/TRACKING/GEOFENCE/STORAGE
surface was physically exercised on the corrected firmware; the unchanged
disconnect/reconnect lifecycle retained the focused M7P7G physical regression
as inherited evidence. M7P7I therefore starts from the merged M7P7H baseline.

## 1. Why this slice exists

The product question is not "does the GNSS parser currently have a fix?" It is:

> Where was this device last validly observed, by which source, when was that
> observation made, and how old is that accepted product fact now?

Firmware owns device Location. Mapping a device to an animal/person/vehicle
entity remains an app/backend Entity Registry responsibility; M7P7I must not
collapse Device Identity into user/entity identity.

Current firmware already has several correct but deliberately narrower pieces:

- `GnssFix` is a portable GNSS observation;
- `GnssManager::takeFreshFixForTransmission()` owns the existing bounded GNSS
  transmission-acceptance/freshness gate;
- `PositionFlow` preserves store-before-send and TLP v1 POSITION compatibility;
- geofence consumes source-neutral `GeoPointE7 + captured_at_ms` after the
  composition root has accepted an observation;
- M7P7H deliberately omits GET_LOCATION because there is not yet one
  transport-neutral accepted/last-known Location owner.

The next correct trunk step is therefore a small runtime Location owner. It must
not be a second GNSS manager, a history reader, or a transport cache.

## 2. Non-negotiable semantic separation

M7P7I must preserve:

```text
GNSS receiver fix
!= accepted Location
!= "fresh/live" presentation
!= stale last-known Location
!= persisted/history Location
!= active Location Source
!= GNSS power state
```

The existing approximately five-second GNSS fresh-fix limit is a bounded
acquisition/TX acceptance rule. It is **not** the product definition of "live"
and must not become a public Location freshness constant.

A valid coordinate of `0,0` is data, not a missing-value sentinel.

Disconnect, source silence, permission loss, GNSS sleep, or transport loss must
not erase the last valid point. CLEAR is a distinct, explicit future operation.

## 3. Current source boundary

M7P7I is **GNSS-only in production composition**.

The first implementation must insert one neutral handoff at the point where
`main.cpp` already consumes an accepted GNSS observation and passes that same
observation onward to geofence/PositionFlow.

It must **not**:

- call `GnssManager::takeFreshFixForTransmission()` a second time;
- consume a fix earlier than the existing acceptance point;
- change acquisition, timeout, continuation, PVT/DOP pairing or GNSS power;
- alter PositionFlow store-first behavior;
- alter geofence confirmation ordering;
- alter TLP v1 POSITION bytes, sequence allocation or RF scheduling.

Every successful `takeFreshFixForTransmission()` handoff is the existing
composition-root acceptance point for this slice, including the extra fresh
observations requested by a geofence confirmation episode. M7P7I must update
the Location owner exactly once from that already-consumed `GnssFix`, before
the existing geofence path. A normal scheduled observation may then also enter
PositionFlow; an extra confirmation observation intentionally does not.

This distinction matters: HistoryStore admission is not Location validity.
If `PositionFlow::acceptFix()` later fails, store-first still suppresses live
RF transmission exactly as today, but that failure must not retroactively erase
the already accepted runtime Location or geofence observation.

## 4. Minimal runtime value/owner

Exact C++ names are not frozen by this planning document, but the first
implementation should remain small and fixed-memory.

A source-neutral accepted-location value needs only facts that are genuinely
owned now:

- latitude/longitude in the existing E7 coordinate convention;
- altitude where available, with explicit availability rather than a magic
  sentinel; for the first GNSS adapter, every promoted `GnssFix` already
  passed the existing `invalidLlh` rejection, so altitude is available even
  for an accepted 2D fix; `kPositionFlag3dFix` remains a quality/fix-type fact,
  not an altitude-validity bit;
- source provenance: GNSS for this slice. Provenance is not active-source
  selection intent; M7P7I does not yet own GNSS-vs-PHONE arbitration;
- a local monotonic observation-time anchor suitable for truthful age across
  normal long-lived tracker uptime. Do not expose raw 32-bit
  `captured_at_ms` as an indefinitely valid product age: it wraps in about
  49.7 days. The implementation should minimally extend the existing
  loop-owned monotonic clock to 64-bit and reconstruct the accepted PVT capture
  time from the bounded (<5 s) 32-bit handoff delta, without changing existing
  GNSS/PositionFlow 32-bit scheduling APIs;
- observation UTC time when the source supplied trustworthy UTC, with explicit
  validity; the GNSS adapter must use the existing
  `kPositionFlagValidUtcTime` semantic rather than infer validity from
  `utc_epoch_seconds != 0`;
- an explicit `has_location`/validity fact.

GNSS-specific diagnostics such as HDOP, satellite count, PVT/DOP session state
and TTFF remain GNSS/Tracking diagnostics. Do not force those fields into the
generic Location model merely because GNSS is the first producer.

The owner is runtime RAM state only in M7P7I. A new owner after reboot starts
UNKNOWN. It must not recover a HistoryStore record and silently present it as a
current accepted Location.

## 5. Update rules

For the first GNSS-only runtime owner:

1. Initial state is UNKNOWN / no accepted location.
2. The owner updates only from an observation already accepted by the existing
   composition root.
3. A later accepted GNSS observation replaces the prior runtime accepted
   location.
4. Rejected/invalid source input never clears or partially overwrites the prior
   accepted location.
5. The owner never mutates HistoryStore, ConfigStore, GeofenceStore or
   SecurityStore.
6. Reading the owner performs no I/O and no O(N) scan.
7. The owner retains a rollover-safe local observation-time anchor. The owner
   does not label an observation "live" merely because it is younger than five
   seconds. The existing five-second rule remains only GNSS admission/live-TX
   evidence, not product freshness policy.
8. Boot/reset creates a new UNKNOWN runtime owner unless a later separately
   reviewed persistence slice establishes trustworthy last-known semantics.

## 6. PHONE/source-switch direction — design now, do not implement now

PHONE/MANUAL/fixed sources are explicitly outside the first implementation, but
their ownership rules must be clear enough that today's GNSS owner is not built
into a corner.

Conceptual future transition behavior:

| Situation | Active-source intent | Last valid Location |
| --- | --- | --- |
| no owner yet, accepted GNSS arrives | GNSS may become active under current GNSS-only policy | replace with GNSS observation |
| PHONE is explicitly selected, valid PHONE observation arrives | PHONE | replace with PHONE observation |
| PHONE selected, invalid/empty update | PHONE remains selected | preserve prior valid Location |
| PHONE selected, phone disconnects/silences/permission lost | PHONE remains selected but unavailable/stale | preserve prior valid Location; do not let GNSS silently steal ownership |
| explicit switch PHONE -> GNSS | GNSS | only a subsequently accepted GNSS observation can become the new Location |
| GNSS unavailable/sleeping | GNSS intent unchanged | preserve prior valid Location as last-known/stale according to consumer policy |
| explicit future CLEAR | policy-defined | clear only through that authorized operation |

Before PHONE can be enabled in production, source intent/generation and
out-of-order session handling require their own reviewed implementation. M7P7I
must not invent that machinery prematurely.

## 7. Relationship to geofence

Geofence already has the correct source-neutral consumption shape:
`GeoPointE7 + captured_at_ms`, with GNSS quality inputs currently used by the
existing confirmation representative-selection policy.

M7P7I must not rewrite the proven geofence state machine.

For the GNSS-only first slice, one consumed accepted observation fans out
without a second GNSS read:

```text
successful takeFreshFixForTransmission()
        |
        +--> Location owner (every accepted observation)
        |
        +--> existing geofence confirmation path
        |
        +--> PositionFlow/store-first path (normal scheduled observation only)
```

The Location update occurs before geofence processing so a geofence-triggered
request for another observation cannot create an ownership/order ambiguity.
This does not change the geofence state machine or its representative-fix
storage behavior.

Future PHONE support must review the GNSS-specific HDOP/satellite representative
ranking before PHONE observations are allowed to drive geofence confirmation.
Do not fabricate satellite/HDOP fields for non-GNSS sources.

## 8. Relationship to PositionFlow / History / TLP v1

M7P7I does not replace PositionFlow.

PositionFlow continues to own:

- legacy POSITION encode path;
- HistoryStore sequence/append admission;
- store-before-send;
- current five-second live TX expiry;
- RF send admission.

HistoryStore remains history/backlog persistence, not the live Location owner.

No M7P7I implementation may reinterpret a recovered history record as fresh
Location after boot. Durable last-known Location is a later versioned storage
decision with explicit observation-time semantics.

TLP v1 remains GNSS POSITION only and byte-identical.

## 9. GET_LOCATION is a follow-up, not this owner slice

M7P7I first establishes trustworthy product state. It does **not** need to expose
a new USB/BLE opcode in the same commit.

After the owner is implemented and validated, a later small application-surface
slice may add GET_LOCATION through the same `ApplicationRequestService` used by
M7P7H.

That later read must distinguish at least:

- UNKNOWN;
- accepted runtime Location;
- source;
- observation/capture-time facts;
- age/freshness presentation semantics.

It must not call GnssManager directly from USB/BLE and must not reconstruct
"current location" by scanning HistoryStore.

Private-location deployment policy must also be revisited before broad
pre-authorization exposure; M7P7H's open BLE status decision is not blanket
authorization for coordinates.

## 10. Explicit non-goals

M7P7I does not implement:

- GET_LOCATION wire bytes;
- PHONE/MANUAL/fixed input;
- BLE phone-location writes;
- source switching/AUTO arbitration;
- persistent last-known Location;
- history migration or a new flash partition;
- NODE_LOCATION RF packets;
- TLP v2;
- gateway/backend location synchronization;
- map UI;
- generic multi-source framework/HAL;
- new GNSS quality policy;
- geofence state-machine rewrite;
- changes to TLP v1 POSITION.

## 11. Required host coverage for the future implementation

The implementation slice must prove at least:

- new owner starts UNKNOWN;
- one accepted GNSS observation produces one accepted Location;
- latitude/longitude/altitude/time/source are copied exactly;
- `0,0` remains a valid coordinate when the caller has accepted it;
- later accepted observation replaces the previous one;
- no invalid/rejected update path erases the prior valid Location;
- accepted capture time immediately across a 32-bit millis rollover is
  reconstructed correctly into the rollover-extended local time;
- a long-lived runtime cannot make an old Location appear newly fresh merely
  because the legacy 32-bit millisecond clock wrapped;
- construction/reboot starts UNKNOWN and does not read HistoryStore;
- owner reads are O(1), side-effect free and fixed-memory;
- existing geofence confirmation tests remain unchanged/passing;
- existing PositionFlow/TLP v1 golden bytes remain unchanged;
- production startup failure scenarios remain passing.

A focused production-composition test must prove that the owner is updated from
the **same accepted GNSS observation** already consumed by geofence/PositionFlow,
not from a second parser/queue read.

## 12. System impact expected from the first implementation

```text
TLP v1 bytes:                  unchanged
RF / airtime:                  unchanged
GNSS acquisition/power:        unchanged
HistoryStore format/wear:      unchanged
Config/Geofence/Security:      unchanged
BLE UUID/framing:              unchanged
M7P7H status wire:             unchanged
RAM:                           one small bounded runtime value/owner plus
                               existing-clock 64-bit exposure
Flash:                         code only; no new persistent allocation
Physical behavior:             no new radio/GNSS behavior expected
```

Because production composition changes which owner observes an accepted GNSS
fix, the implementation still requires full host/startup validation and a
RAK4630 production build. Physical GNSS qualification should be required only
if review finds the insertion changed acquisition/consumption ordering; the
slice is intentionally designed so it should not.

## 13. Gate ordering

M7P7H is already merged. Safe order from this baseline:

```text
review/merge M7P7I owner contract
-> implement M7P7I owner on latest main
-> host + warnings/sanitizers + startup
-> RAK4630 build
-> independent audit
-> focused physical test only if ordering/runtime evidence requires it
-> merge
-> later GET_LOCATION application-surface slice
```

This keeps the Location trunk small and reviewable without folding transport
exposure, persistence or future PHONE source arbitration into the first owner.


## 14. Implementation candidate

PR #63 implements only the owner foundation frozen above.

Current production changes:

- new `LocationOwner` / `AcceptedLocation` fixed-memory runtime value;
- current source provenance values are only `UNKNOWN` and `GNSS`; these are
  internal runtime semantics, not frozen wire IDs or active-source policy;
- legal latitude/longitude and non-UNKNOWN provenance are defended before owner
  replacement; rejection preserves the prior accepted Location;
- every successful production `takeFreshFixForTransmission()` publishes that
  exact `GnssFix` to the Location owner before geofence processing;
- geofence confirmation-only extra observations therefore update latest
  Location while still bypassing the normal PositionFlow branch exactly as
  before;
- normal PositionFlow/HistoryStore failure cannot retract an already accepted
  runtime Location;
- GNSS UTC validity maps from the existing
  `kPositionFlagValidUtcTime` bit; zero epoch remains a value, not the
  validity sentinel;
- promoted GNSS fixes set `altitude_valid=true` because the existing manager
  rejects NAV-PVT `invalidLlh`; the separate 3D-fix bit remains quality/type,
  not altitude validity;
- the existing loop-owned `TickMillis` clock now exposes one shared
  rollover-extended `nowMs64()` while `nowMs()` retains its exact modulo
  32-bit scheduling interface;
- the recent GNSS callback timestamp is extended into that 64-bit timeline from
  the already-bounded fresh-fix delta, so a long-lived runtime cannot make an
  old Location appear young merely because the legacy millisecond clock
  wrapped.

Still deliberately absent:

- GET_LOCATION over USB/BLE/LoRa;
- PHONE/MANUAL/fixed location input;
- active-source arbitration/generation;
- durable last-known Location;
- new flash allocation/format;
- any TLP v1/v2 wire change.

Added validation coverage:

- focused `test_m7p7i_location_owner.cpp`;
- focused `test_m7p7i_source_contract.py`;
- production startup checks for UNKNOWN-after-reboot, ordinary GNSS publication
  and confirmation-only observation replacement;
- the existing full host/startup suite continues to be the merge gate.

A local compiler smoke check of the isolated new owner/clock primitives passed
with both `-std=gnu++11 -Wall -Wextra -Werror` and C++17
ASan/UBSan. This is developer syntax/unit smoke only; it is **not** a substitute
for the repository full host suite, production RAK4630 build, independent audit
or physical evidence.
