# Application Surface Architecture Review — Audit Disposition

Status: **INDEPENDENT ARCHITECTURE REVIEW DISPOSITION — DOCUMENTATION-ONLY.**

Reviewed baseline:
`main@034d0afdbd7b4e26fd2cd44310486f20a61307d7`.

Review date:
2026-10-02.

Governing direction after disposition:
`docs/architecture/ORUN_APPLICATION_TRANSPORT_SURFACE.md`.

Next implementation contract:
`docs/milestones/M7P7H.md`.

The independent review was performed read-only. It did not run the repository
host suite, production build or physical hardware.

## 1. Verdict

**Architecture direction sound; focused corrections required before growth.**

The review accepted the existing canonical direction that USB, BLE and LoRa are
transports into common application/domain owners. It found no need to redesign
the product architecture.

It identified five near-term seams that must be handled before or during the
next application-surface slice:

1. do not grow one flat ApplicationResponse struct into a monolith;
2. add an access/security-context seam distinct from requester provenance;
3. keep response delivery slot separate from durable mutation serialization;
4. do not inject concrete GNSS/driver dependencies into ApplicationRequestService;
5. do not enable writers until store admission/completion/result contracts can
   represent real asynchronous/ambiguous outcomes.

## 2. Existing design accepted and preserved

The review accepted these current invariants:

- one fixed-memory loop-owned application request seam;
- no heap requirement;
- fail-closed unsupported request behavior;
- requester provenance is not user identity or authorization;
- bounded BLE callback -> loop handoff;
- session-generation and stop-and-wait behavior;
- GET_CONFIG wire/UUID compatibility;
- ResourceState != TokenState for durable stores;
- store-owned state-token progression rather than peer-selected new tokens;
- USB/BLE convergence on one application owner;
- future LoRa/gateway connection to the same target-side owner.

## 3. Accepted corrections

### 3.1 Response shape

Current GET_CONFIG-specific fields must not be extended indefinitely.

Accepted direction:

```text
common response header
+ bounded payload selected by response kind
```

Payloads should remain POD/fixed-memory and compatible with the repository's
gnu++11 toolchain.

### 3.2 Snapshot boundary

ApplicationRequestService must not accumulate `GnssManager&`,
`RadioManager&`, driver headers or Arduino dependencies.

Existing owners/composition provide bounded read snapshots. The application
service transports/dispatches those snapshots.

### 3.3 Access context

The current requester enum answers local adapter provenance only.

A separate application-boundary access context is required before more
operations exist. Adapters provide facts such as local USB, BLE link security
state or later authenticated LoRa scope. The application boundary owns the
operation-admission decision.

This is not final authorization and does not replace the reviewed security
architecture.

### 3.4 Read response vs mutation lifetime

The current single response slot is adequate bounded backpressure for the first
synchronous read-only families.

It must not become the mutation lock.

A mutation may outlive the local requester transaction and must be serialized by
the relevant domain owner. Read-only status must not become globally BUSY for
the duration of an unrelated durable write merely because a response slot is
used as a lock.

### 3.5 Store result APIs before writers

Current bool-style save/result APIs are insufficient for a product mutation
surface because they can collapse materially different states.

Before production SET_CONFIG/geofence mutation, the application layer must be
able to distinguish typed admission/completion outcomes including unchanged,
busy, invalid, maintenance/unavailable, stale/precondition failure,
confirmed failure and OUTCOME_UNKNOWN/reconciliation.

Exact enums remain implementation work.

## 4. M7P7H recommendation accepted

The smallest next implementation slice is a read-only Device Status application
surface over USB and BLE with four families:

- DEVICE;
- TRACKING/GNSS;
- GEOFENCE;
- STORAGE.

It deliberately excludes GET_LOCATION, writers, LoRa adapter, security runtime,
battery placeholder and bulk resource transfer.

The rationale is to fill the common target-side application surface before
binding future LoRa COMMAND/RESULT/store-forward to it.

## 5. GET_LOCATION disposition

GET_LOCATION is deferred from M7P7H because current runtime does not yet provide
a final accepted/last-known Location owner suitable for a product surface.

The review requires the future design to keep separate:

- raw/receiver GNSS fix;
- accepted Location;
- product freshness/age;
- stale last-known Location;
- persisted/history Location.

The current 5-second GNSS fresh-fix threshold must not be exported as the
product definition of live location.

## 6. Current USB command disposition

The review accepted that transport symmetry applies to product semantics, not
all engineering commands.

Product observations may absorb selected existing USB facts, while volatile role
override, raw sensor probes, ACTIVITY START, BLE implementation diagnostics and
qualification probes remain USB/test-only unless a later product requirement
explicitly promotes them.

## 7. Writer ordering constraints

Before the first SET_CONFIG family, resolve the persistent requested
role/profile/relay configuration scope so the first command payload is not
invalidated immediately by the next schema step.

Before the first production geofence writer, production polling, typed mutation
result/token semantics, live runtime replacement/reconciliation, first-baseline
maintenance recovery, alarm lifecycle and rate limiting must be closed.

These findings do not expand M7P7H.

## 8. LoRa/gateway disposition

Future LoRa requirements must influence today's seam:

- bounded request/result families;
- access/scope context;
- local request_id distinct from future command_id;
- mutation result not tied to one live local session;
- no generic "FAILED" where outcome is uncertain;
- target-side domain owner remains authoritative.

The following remain intentionally unimplemented in M7P7H:

- LoRa application adapter;
- secure envelope;
- replay slots;
- command scopes/opcodes;
- custody/store-forward;
- gateway runtime;
- resource-transfer wire format.

## 9. Evidence boundary

This audit disposition is architecture/source-review evidence only.

It does not establish:

- host-test PASS;
- sanitizer/warnings PASS;
- RAK4630 build PASS;
- physical BLE PASS;
- RF/power behavior;
- production security;
- mutation power-cut behavior.

Those gates belong to the later implementation milestone.
