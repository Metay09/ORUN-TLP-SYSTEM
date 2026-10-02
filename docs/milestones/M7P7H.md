# M7P7H — Read-only Device Status application surface (USB + BLE)

Status: **IMPLEMENTATION CANDIDATE — VALIDATION PENDING; NOT MERGE-READY.**

Implementation baseline:
`main@8f5f8b75e2b5c27d10e4dfec3130267afc300314`
(PR #60 application-surface direction merged).

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

## 8. Candidate implementation contract

The current implementation candidate is on
`feat/m7p7h-read-only-device-status`.

Internal typed request kinds are additive:

```text
1 GET_CONFIG
2 GET_DEVICE_STATUS
3 GET_TRACKING_STATUS
4 GET_GEOFENCE_STATUS
5 GET_STORAGE_STATUS
```

`ApplicationRequester` remains USB/BLE only in this slice. A separate
`ApplicationAccessContext` now carries local channel/security facts; all five
current operations are read-only local operations and are admitted for USB
local, BLE open and BLE encrypted contexts. This is an insertion seam, not
final authorization.

The application response is now a common header
`(requester, request_id, kind, code)` plus one bounded kind-specific POD
payload. `ApplicationRequestService` still depends only on ConfigStore plus
the bounded application snapshot; concrete GNSS/radio/Arduino driver headers
remain outside it.

Candidate BLE message types are additive on the existing UUID/20-byte framing:

```text
request   response
0x01      0x81   GET_CONFIG (unchanged)
0x02      0x82   DEVICE
0x03      0x83   TRACKING/GNSS
0x04      0x84   GEOFENCE
0x05      0x85   STORAGE
0xFF             ERROR
```

New ERROR values are additive:

```text
0x01 UNSUPPORTED (unchanged)
0x02 BUSY        (unchanged)
0x03 ACCESS_DENIED
0x04 UNAVAILABLE
```

Every M7P7H request has zero logical payload. Existing GET_CONFIG response bytes
remain unchanged.

Candidate response logical payloads:

### DEVICE — 20 bytes

```text
0      status
1      application-surface revision
2      legacy role observation
3      flags: bit0 AUTO, bit1 watchdog-reset
4      GNSS presence
5      GNSS health
6      accelerometer presence
7      accelerometer health
8      tracking effective state
9      tracking reason
10     relay-forwarding effective state
11     relay-forwarding reason
12..15 uptime_ms LE32
16..19 reset_reason LE32
```

### TRACKING/GNSS — 36 bytes

```text
0      status
1      flags: bit0 config-ready, bit1 stored semantic override,
              bit2 GNSS detected, bit3 additional-fix active
2      geofence cadence mode
3      GNSS state
4..7   requested durable base interval seconds
8..11  applied-at-boot base interval seconds
12..15 actual GnssManager runtime interval seconds
16..19 acquisition attempts
20..23 successful fresh fixes
24..27 acquisition timeouts
28..31 invalid fixes
32..35 last TTFF ms
```

The actual GnssManager interval is exposed rather than re-deriving B/B3. A
failed future cadence apply must therefore remain observable as an applied-state
difference rather than being hidden by an expected-value calculation.

### GEOFENCE — 9 bytes

```text
0      status
1      durable resource state
2      token-state summary (not token bytes)
3      flags: bit0 runtime configured, bit1 confirmed state available,
              bit2 confirmation active
4      area count
5..6   total effective vertex count LE16
7      confirmed operational state (UNKNOWN/INSIDE/OUTSIDE)
8      cadence mode
```

GeofenceStore exposes only O(1) area/vertex summary accessors for this query.
The durable geometry is not copied.

### STORAGE — 40 bytes

```text
0      status
1      flags: history ready/busy, config ready/maintenance,
              geofence ready/maintenance, security ready/exhausted
2      SecurityStore state
3      reserved=0
4..7   history count
8..11  history capacity
12..15 overwritten
16..19 history append failures
20..23 history recovery corruptions
24..27 history metadata failures
28..31 config recovery corruptions
32..35 geofence recovery corruptions
36..39 security recovery corruptions
```

Routine status uses O(1) RAM state/counters only. It does not invoke
`HistoryStore::newest()`, `backlogCount()`, or
`GeofenceStore::currentSnapshot()`.

USB command spellings are:

```text
APP CONFIG?
APP DEVICE?
APP TRACKING?
APP GEOFENCE?
APP STORAGE?
```

USB formatting is isolated in `usb_application_adapter.cpp`; existing
engineering commands remain outside the product application surface.

No validation result is claimed yet. Host/sanitizer/startup, RAK4630 build,
independent audit and physical BLE qualification remain open gates.

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

## 11. Evidence boundary

The implementation candidate changes only the local read-only application
surface and additive BLE application message types. It does not change TLP v1,
RF behavior, persistent flash formats, GNSS power policy or enable a writer.

At this point no host/build/physical PASS is claimed. Evidence is added only
after it is actually run against the candidate head.
