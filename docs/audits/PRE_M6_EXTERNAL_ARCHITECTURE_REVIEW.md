# Pre-M6 Independent Architecture Research Review

Date: 2026-09-15
Reviewed branch documentation head: `f2c79b418f3886599a55a372ca45d2b789068edc`
Code-bearing baseline: `a9d7bde7afcce2d7a34912c6fc4cdfcbf1788986`
Status: **independent research/audit input; no code or hardware validation was performed by this document**.

## Verdict

**GREEN WITH CONDITIONS**.

The independent reviewer found no P0/Critical architecture defect requiring ORUN
to stop, replace its architecture, move to an RTOS, introduce a large HAL, or
rewrite the existing GNSS/radio/storage state machines before B4/M6.

The review supports the current direction:

- one product firmware codebase on the RAK reference platform;
- identity/profile/capability/location/power/transport separation;
- relay forwarding independent from application services;
- gateway bridging independent from LoRa relay forwarding;
- GNSS as a source-specific observation rather than the Location concept itself;
- requested configuration separated from effective runtime state;
- TLP v1 byte compatibility frozen;
- store-before-send;
- callback handoff + single radio owner + bounded queues;
- no durable config before flash/BLE/InternalFS/DFU ownership is proven.

## Accepted FIX NOW / pre-M6 findings

### Capability presence must not be boolean

Use at least the semantics UNKNOWN / PRESENT / ABSENT, with health tracked
separately. A transient bus failure, rail-off condition or unprobed sensor is not
proof of physical absence.

Recommended product interpretation:

- ABSENT: hide normal controls;
- PRESENT + OK: show normally;
- PRESENT + FAULT/DEGRADED: show as installed but degraded;
- UNKNOWN/UNPROBED: do not claim absence.

### Requested intent and effective state must be different

B4 should preserve a valid requested intent even when hardware cannot currently
satisfy it. The effective state must carry a reason for blocked/degraded service.
Invalid configuration is rejected; valid but currently unsatisfied intent is not
silently rewritten.

One-shot command execution is different: future commands against unavailable
actuators must be rejected rather than retained as latent configuration intent.

### Relay forwarding is not yet independent in production runtime

B3 introduced `LegacyRoleBehavior::relay_forwarding_enabled`, but the production
network path still branches on `NodeRole` in `NetworkService`. This is consistent
with B3's documented bounded scope, but a user-facing independent relay toggle
must not be shipped until the runtime forwarding decision consumes resolved/
effective behavior instead of the raw role enum.

`receive_application_position` should remain a legacy compatibility projection
for now, not a frozen public configuration axis, because future collector,
gateway and subscriber semantics may differ.

### Power policy may degrade service only explicitly and observably

The invariant is not "relay can never turn off". The invariant is that power
policy cannot silently rewrite user intent. A future user-selected survival policy
may authorize observable relay suspension at critical battery; a required relay
availability policy may not.

### Current history capacity is below the aspirational 1–2 week target

The present seven-page journal contains 728 records, approximately 7.58 days at
a 15-minute report interval. Documentation must distinguish this measured/current
capacity from the longer product target.

### M6 LOST network contact remains security/receipt-gated

Local geofence/activity work may proceed. A trustworthy "no network contact"
LOST condition cannot be claimed from TX_DONE or unauthenticated v1 traffic.
Authenticated contact/receipt semantics are required before that product claim.

## KEEP findings

The reviewer recommends preserving, not rewriting:

- B1 compatibility freeze and literal golden fixtures;
- B2 `DeviceIdentity`, `GnssFix` and pure legacy POSITION mapping seams;
- B3 compatibility projection direction;
- current GNSS freshness/PVT+DOP/session logic;
- PositionFlow store-before-send semantics;
- current flash journal/recovery behavior;
- cooperative state machines;
- callback-only handoff and single radio owner;
- fixed/bounded queues;
- host/build/hardware evidence separation;
- persistent-config delay until storage ownership is resolved;
- M6 activity + local geofence direction.

`GnssFix` must remain understood as a portable **GNSS** observation, not a generic
PHONE/MANUAL Location type.

## DEFER findings

Do not implement during B4/M6 unless a concrete dependency changes:

- TLP v2;
- multi-hop/routing tables;
- LoRaMesher/Reticulum-style network stack replacement;
- large generic HAL/event bus/plugin system;
- second-board support;
- BLE;
- durable config flash;
- generic Location model before a second source exists;
- remote commands/actuation;
- messaging;
- backend/mobile application;
- generic telemetry format solely for future-proofing.

## Network-scale conclusion

The independent research compared ORUN with Meshtastic, MeshCore, LoRaMesher,
Reticulum, MySensors, OpenThread, Bluetooth Mesh and DTN/BPv7 concepts.

The useful external pattern is separation of forwarding, bridging, application
and low-power responsibilities. No external stack should be copied wholesale.

For current small fleets, direct + fixed one-hop relay remains appropriate.
Future larger networks should investigate a hybrid controlled approach:

- stable network message identity;
- bounded dedupe;
- small hop budget;
- controlled/randomized forwarding for discovery/fallback;
- preferred powered relay infrastructure;
- learned/direct paths when useful;
- bounded store-forward during disconnection;
- RF domains/gateways/backhaul rather than one flat 1000-node flood domain.

The existing SF11/BW125 airtime makes a flat 1000-node single-domain design
physically unrealistic before collision/retry overhead is even considered. This
is a future network-design input, not a reason to change B4/M6.

## Security conclusion

Before protected person location, remote configuration, private messaging or
actuation, require reviewed mature security providing the appropriate combination
of authentication, authorization, anti-replay, confidentiality, expiry and
idempotency/result semantics. Network security and end-to-end application privacy
are separate concerns. Relays/gateways need not see private application plaintext.

Do not invent cryptography and do not enable remote command/actuation as an
unauthenticated legacy fallback.

## Recommended sequence

```text
B2/B3 exact regression + final audit/merge gate
        ↓
B4 requested config + capability presence/health + effective state/reason
        ↓
M6 activity + local geofence
        ↓
pre-M7 flash/SoftDevice/InternalFS/bond/DFU ownership
        ↓
M7 BLE/provisioning/config/DFU
        ↓
M8 Android/gateway/backend
        ↓
future security/network/control milestones
```

The review's final recommendation is to continue the existing pre-M6 architecture
hardening direction, not restart the architecture.
