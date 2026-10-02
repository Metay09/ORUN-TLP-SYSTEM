# M7P7H — Read-only Device Status application surface (USB + BLE)

Status: **PLANNED — ARCHITECTURE/ACCEPTANCE CONTRACT ONLY; NO RUNTIME IMPLEMENTATION IN THIS DOCUMENT.**

Baseline for planning:
`main@034d0afdbd7b4e26fd2cd44310486f20a61307d7`.

Governing architecture:
`docs/architecture/ORUN_APPLICATION_TRANSPORT_SURFACE.md`.

Independent architecture review disposition:
`docs/audits/APPLICATION_SURFACE_ARCHITECTURE_AUDIT_DISPOSITION.md`.

## 1. Goal

Expand the existing transport-neutral application seam beyond GET_CONFIG
without creating BLE-specific or USB-specific product logic.

The slice provides the first useful phone/USB product-service surface after
M7P7G:

```text
USB adapter ----\
                 +--> common typed read-only application surface
BLE adapter ----/
                 |
                 +--> DEVICE
                 +--> TRACKING/GNSS
                 +--> GEOFENCE
                 +--> STORAGE
```

LoRa is deliberately not implemented in this slice. The common seam must remain
compatible with a later authenticated/store-forward LoRa adapter.

## 2. Hard scope

Implement:

- response header + kind-specific bounded POD payloads rather than one flat
  ever-growing response struct;
- a small transport/access-context seam at the application boundary;
- no direct driver/Arduino dependencies inside ApplicationRequestService;
- bounded typed snapshots assembled in loop/application context;
- DEVICE read family;
- TRACKING/GNSS read family;
- GEOFENCE read family;
- STORAGE read family;
- USB adapter formatting for those families outside domain owners;
- additive BLE request/response types on the existing M7P7F/M7P7G GATT service;
- GET_CONFIG wire/behavior compatibility;
- fail-closed unknown/malformed request handling;
- focused host/source-contract/startup tests.

Exact new BLE type values are frozen only by the implementation/golden tests of
this milestone, not by this planning document.

## 3. Non-goals

Do not implement:

- GET_LOCATION or a new Location owner;
- geofence geometry read;
- raw config/geofence token-byte read;
- SET_CONFIG;
- GeofenceStore writer;
- `ApplicationRequester::kLora`;
- TLP v2 application adapter;
- COMMAND/RESULT runtime;
- replay/custody/store-forward;
- gateway runtime;
- battery percentage/voltage placeholder without a real measurement owner;
- recent-event ring or generic Health framework;
- generic StatusProvider interface;
- BLE transport v2 or MTU redesign;
- new GATT characteristics;
- commissioning or final authorization;
- BLE exposure of legacy role mutation or ACTIVITY START;
- any TLP v1, RF, flash-format or GNSS power change.

## 4. Snapshot rules

All four families are snapshots, not new owners of domain truth.

Rules:

- assemble in loop/application context;
- use one captured monotonic `now` for all age calculations in one response;
- adapters do not independently calculate freshness/age;
- do not copy full geofence geometry to the loop stack;
- do not run O(N) HistoryStore scans for routine status;
- preserve requested vs applied vs effective tracking values;
- do not map the current 5-second fresh-fix threshold to a product "live" flag;
- preserve Geofence ResourceState vs TokenState distinction.

## 5. Current USB command disposition

The slice may consume existing USB evidence but must not blindly promote every
Serial command to product API.

| Existing USB behavior | M7P7H disposition |
| --- | --- |
| APP CONFIG? | preserve through common application seam |
| ROLE? | expose observable role/mode in DEVICE |
| ROLE TRACKER/RELAY/BASE | engineering USB only; no BLE product mutation |
| RADIO? | expose only product-level effective/listen state where owned; detailed timing counters stay engineering |
| BLE? | engineering USB diagnostic |
| ACCEL? | capability presence/health may enter DEVICE; raw XYZ probe stays engineering |
| ACTIVITY? / ACTIVITY START | engineering USB diagnostic |
| CRYPTO* / FLASH PROBE | test/qualification image only |

## 6. BLE constraints

Preserve the M7P7F/M7P7G physical/application transport contract:

- same service/request/response UUIDs;
- request WRITE-with-response;
- response INDICATE;
- 20-byte physical frame;
- <=48-byte logical payload for this slice;
- same bounded callback->loop handoff;
- same session generation/stop-and-wait/HVC rules;
- no application/flash/Serial/radio/clock work in BLE callback context.

The first status families must fit the existing bounded transport. Large
resources remain a later resource-transfer problem.

## 7. Access-context rule

Requester provenance and access/security context are separate.

Adapters report channel/security facts. The application boundary owns the
operation-admission table.

This slice does not implement final user authorization. It must merely avoid
hard-coding product operation allow-lists independently in USB and BLE adapters.

## 8. Acceptance criteria

M7P7H closes only when all are true:

1. Existing GET_CONFIG USB output and BLE golden bytes remain compatible.
2. Application response shape is common header + bounded kind-specific POD
   payload; ApplicationRequestService has no GNSS/Arduino/driver-header
   dependency.
3. One central operation-to-access decision path exists; adapters provide
   context facts rather than separate product allow-lists.
4. DEVICE, TRACKING/GNSS, GEOFENCE and STORAGE come from the same typed
   snapshots for USB and BLE.
5. Requested base interval, applied base interval and effective runtime interval
   are observably distinct.
6. GEOFENCE summary exposes no geometry and no token bytes.
7. Routine STORAGE status performs no O(N) history scan.
8. Unknown request type and malformed payload remain fail-closed.
9. Full host suite including warnings-as-errors and sanitizers passes.
10. Production startup scenarios pass.
11. RAK4630 production build passes; RAM/flash delta is recorded.
12. Physical RAK4631 + Android/nRF Connect reads each new family and repeats
    GET_CONFIG/HVC/disconnect-reconnect regression.
13. Milestone and architecture docs are updated to actual implemented bytes and
    evidence; no unperformed physical result is claimed.

## 9. Writer prerequisites kept out of this slice

Before SET_CONFIG:

- persistent requested role/profile/relay configuration scope must be resolved
  so the first config command is not immediately invalidated by a schema
  expansion;
- one ConfigService/domain mutation owner must own serialized mutation lifetime;
- ConfigStore result API must distinguish typed admission/completion/
  OUTCOME_UNKNOWN and return the resulting state token;
- requested/applied/effective timing must be explicit;
- production maintenance/re-baseline behavior must be available;
- final write authorization is a later security slice.

Before the first production geofence writer:

- `geofence_store.poll()` must be serviced in normal production loop under the
  existing flash/radio ownership constraints;
- typed mutation admission/completion/result-token semantics are required;
- live replacement/CLEAR must cancel/re-anchor current confirmation/runtime
  state safely;
- runtime must reconcile durable OUTCOME_UNKNOWN state before presenting a
  result;
- large staging remains non-authoritative until final validated commit;
- first-baseline maintenance/recovery and alarm lifecycle semantics must be
  resolved;
- mutation rate limiting must be defined.

These are writer prerequisites, not M7P7H implementation scope.

## 10. Evidence boundary

This planning document itself changes no firmware/runtime/protocol behavior and
claims no host/build/physical PASS.
