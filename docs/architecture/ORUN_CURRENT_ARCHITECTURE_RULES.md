# ORUN Current Architecture Rules

Status: **CURRENT pre-M6 owner-approved architecture rules**.
Last reviewed against code: `3186c96d54f0f84243296a776a0f1ffa5ed4c526`.
Last architecture review update: 2026-09-15.
Scope: concept boundaries and ownership; this file does not authorize new wire,
storage, BLE, security, sensor-driver or multi-hop implementation by itself.

This document exists so a new engineer can distinguish the behavior that is
physically/currently implemented from the product architecture we are building
toward. `AGENTS.md` remains the project instruction source. Older architecture
documents remain useful design/audit records, but silent contradictions with
these owner-approved rules must be removed when the affected area is changed.

## 1. Non-negotiable concept separation

These are separate facts and must not be collapsed into one enum or inferred
from one another:

`Role != Location Source != GNSS Power != Capability != Transport != Identity != Profile != User Identity`.

Also keep separate:

- relay forwarding responsibility;
- gateway bridging;
- application services such as tracking, telemetry, sensing and actuation;
- hardware support and physical presence;
- hardware health;
- requested configuration;
- effective runtime state;
- system power policy.

A profile is only a versioned preset/default bundle. It is not a permanent
hardware restriction or topology identity.

## 2. One product firmware codebase; configuration decides behavior

ORUN uses one firmware codebase on the current RAK4630/RAK4631 reference
platform. We do not build separate tracker, relay, gateway or sensor firmware
projects.

"One firmware" means one product firmware codebase and one set of application,
configuration and protocol semantics. It does not require one identical binary
for every future MCU/board. If a real second platform is introduced later,
board-specific build artifacts are acceptable while the product semantics remain
shared and portable boundaries remain small.

A future provisioned device is composed from independent settings such as:

```text
application services: tracking / telemetry / sensing / actuation / activity ...
relay_forwarding_enabled: true | false
gateway_bridge_enabled: true | false
location source/policy: GNSS / PHONE / MANUAL / NONE / reviewed AUTO
GNSS power policy: separate
system power policy: separate
```

Legacy TRACKER/RELAY/BASE remains a compatibility surface until an explicit
migration replaces it. New application families must not extend that enum.

## 3. Relay forwarding is not a device type

Relay forwarding means forwarding **other nodes' eligible packets**. A node's
own telemetry, position or event is originated application traffic, not relay
traffic.

Therefore valid future combinations include:

- animal tracking + relay forwarding;
- humidity/temperature telemetry + relay forwarding;
- valve/actuation service + relay forwarding;
- pure relay with minimal application services;
- gateway bridge with relay forwarding ON or OFF.

Enabling relay forwarding must never disable the node's application services.
The animal-tracker preset defaults relay forwarding OFF because continuous RX and
extra TX cost battery and airtime. That default is not a universal prohibition;
a user may explicitly enable forwarding when the validated configuration surface
exists.

When relay forwarding is enabled, continuous LoRa RX between local transmissions
is an availability commitment. Because SX1262 is half-duplex, RX pauses during
TX and resumes afterwards.

Power policy must not silently rewrite user intent. A future explicit policy may
permit observable degradation, for example suspending best-effort relay service at
critical battery. A required-availability policy must not be silently overridden.
Any policy-authorized change of effective service must expose a reason/state.

Gateway bridging is separate from LoRa relay forwarding. A gateway can bridge
without repeating RF traffic, relay without gateway service, or do both.

## 4. Current legacy compatibility remains frozen

The current compatibility projection intentionally preserves M5 behavior:

| Legacy role | Relay forwarding | Publish own GNSS POSITION | Receive app POSITION |
| --- | --- | --- | --- |
| TRACKER | OFF | yes | no |
| RELAY | ON | no | no |
| BASE | OFF | no | yes |

This table describes **current compatibility**, not the target product type
system. `RoleController::updateAutomatic()` still performs the legacy unprovisioned
bootstrap:

- GNSS detected -> TRACKER;
- GNSS absent after bounded detection -> BASE;
- USB role override is volatile until reboot.

That heuristic is temporary compatibility behavior. Hardware presence must not
become the future owner of application profile or relay responsibility. Explicit
validated configuration will take precedence once implemented.

B4 has moved actual relay-forwarding service ownership away from the raw role
enum. `NetworkService` owns an explicit `relay_forwarding_enabled` runtime state,
and relay admission, nested-relay rejection and due-forward extraction use that
state. `RadioManager` schedules relay TX from the explicit state and provides a
bounded apply path that preserves the existing driver-gate, callback, TX and
receive-epoch invariants.

Legacy `setRole()` still installs the historical forwarding default so existing
TRACKER/RELAY/BASE behavior is unchanged. This is a compatibility adapter, not a
return to role-owned forwarding. `main.cpp` now resolves the legacy requested
defaults through the B4 RequestedConfig/CapabilitySnapshot/EffectiveConfig path
and applies the resolved relay state through RadioManager. No user-facing relay
toggle or durable configuration exists yet.

`receive_application_position` remains legacy BASE compatibility behavior and is
still role-based inside the network path. It is not yet approved as a general
user-facing configuration field. Future collector/gateway/subscriber semantics
may need a different model, so do not prematurely freeze this boolean into the
public configuration schema.

Current B4 does not define legacy BASE application receive and relay forwarding
as a dual-use mode. If an internal BASE instance is forced to enable forwarding,
forwarding takes precedence in `NetworkService::receive()`. Do not expose a
BASE/collector + relay combination until collector/gateway/subscriber semantics
are explicitly designed and tested.

## 5. Capability model and product visibility

Keep these facts separate:

```text
firmware supports a hardware family
!= hardware presence state
!= hardware health
!= service is requested
!= service is effectively running
```

Minimum presence state for optional hardware:

```text
UNKNOWN   not yet proven present or absent
PRESENT   positively identified/assigned
ABSENT    bounded detection completed with absence result
```

Minimum health is separate from presence, with values such as:

```text
OK
DEGRADED
FAULT
UNAVAILABLE
```

`PRESENT + FAULT` is not `ABSENT`. Rail-off, transient I2C failure, a recovery
attempt, or an unprobed device must not silently make hardware disappear.

Optional digital hardware should be detected with a bounded probe that validates
an identifying/product/protocol response when practical. An I2C address alone is
not sufficient proof when multiple device families can share it. SPI, UART and
1-Wire devices use their equivalent product/family/protocol identity when the
component provides one.

A generic analog input, dry contact or other self-describing-impossible sensor
cannot tell the MCU what physical quantity it represents. Its channel therefore
requires an explicit configured assignment when that feature is implemented.

Normal product UI rule:

```text
ABSENT capability             -> hide normal controls
PRESENT + healthy             -> show normally
PRESENT + fault/degraded      -> show with degraded/fault state
UNKNOWN / UNPROBED            -> do not claim ABSENT
```

This keeps a universal firmware image from filling the UI with hardware that does
not exist while still making a disconnected/failing installed sensor visible as
a fault rather than making it vanish.

Capability does not imply service enablement. A GNSS module may be present while
GNSS tracking is disabled or a different location source owns the active point.

The current B4 composition root marks the RAK product image as supporting GNSS,
uses bounded `GnssManager` detection to distinguish UNKNOWN/PRESENT/ABSENT, and
does not use that capability snapshot as the owner of role, profile or GNSS power.
Acquisition-specific GNSS failures remain owned by the existing GNSS state
machine; they do not make installed hardware disappear. The present composition
therefore provides only a coarse GNSS health projection: detected hardware is
reported as `PRESENT + OK`; acquisition timeout/recovery is not yet aggregated
into the B4 capability health field.

## 6. Requested configuration, effective state and commands

B4 establishes this small boundary without building a large framework:

```text
Profile/default source
      ↓
RequestedConfig
      ↓
validate(candidate)
      ↓
CapabilitySnapshot (support + presence + health)
      ↓
resolve()
      ↓
Effective service state (enabled/blocked/degraded + reason)
```

Requested intent must not be erased because hardware is temporarily unavailable.
For example:

```text
tracking requested = ON
location source = GNSS
GNSS presence = ABSENT
```

is a valid persistent/product intent if the fields themselves are semantically
valid. Its effective runtime result is tracking disabled/blocked with a reason;
it is not an excuse to rewrite requested tracking OFF.

Reject candidate configuration when the configuration itself is invalid, such as
an out-of-range interval or an unsafe cross-field combination. Distinguish that
from a valid intent that cannot currently be satisfied because capability or
health is unavailable. The B4 pure resolver defensively fail-closes all effective
services if an invalid candidate reaches it; a future authoritative configuration
owner must still reject that candidate before replacing the previous requested
state.

One-shot commands are different from configuration intent. A future actuation
command targeting an unavailable actuator must be rejected with an explicit
result; it must not be retained as "requested=true until hardware appears".

The current B4 production composition still derives RequestedConfig from the
frozen legacy role projection. This is intentionally only a migration source.
Tracking effective state gates PositionFlow/fix admission; relay effective state
is applied through the safe RadioManager forwarding boundary. A later validated
configuration surface may replace the legacy source without changing those
service ownership boundaries.

B4 is runtime-only. Do not allocate flash or claim durable configuration until
the verified partition/ownership work is complete.

## 7. Location and GNSS remain separate

GNSS is one hardware/source implementation. Location is the higher-level data
and ownership concept.

- GNSS presence does not imply TRACKER/profile selection in the target model.
- GNSS power state does not imply location validity.
- A missing GNSS device must not turn a provisioned node into BASE/gateway/relay.
- A source disappearing must not erase the last valid point.
- `0,0` is a valid coordinate, never a missing-value sentinel.
- Recovered/persisted data is not automatically live/fresh.

B2 moved the portable GNSS value to `GnssFix` and the frozen legacy POSITION
mapping to a pure boundary. `GnssFix` is a **portable GNSS observation**, not the
future generic Location model; it intentionally still carries GNSS-specific
satellite/HDOP/time semantics. PHONE/MANUAL/fixed location must not be forced into
`GnssFix`. Do not add a generic Location abstraction until a real second source
needs it.

B4 does not change GNSS power ownership. `GnssManager::poll()` and the existing
sensor-rail state machine continue independently of application tracking service
resolution. Do not infer GNSS power from role or tracking enablement without a
separate reviewed power-policy change.

## 8. Network evolution and scale boundary

Current TLP v1 behavior is frozen:

- POSITION is the existing 34-byte v1 packet;
- RELAY_FORWARD is the existing 49-byte v1 one-hop wrapper;
- current forwarding rejects nested relay envelopes;
- current dedupe/queue limits remain as tested;
- TLP v1 bytes and golden compatibility fixtures must not be weakened.

The one-hop rule is a **current v1 compatibility rule**, not a permanent ORUN
architecture limit. Future ORUN may support controlled bounded multi-hop after a
separate network/protocol design and validation milestone. That work must define
stable network message identity, duplicate suppression, hop/flood bounds, airtime
admission, security/authentication, mixed-fleet behavior, reset/cache behavior
and field tests. Do not introduce unlimited flooding or an accidental mesh by
simply forwarding relay envelopes again.

Do not model 1000 nodes as one flat single-channel/SF11 flood domain. Future
large-fleet work must model airtime and collision capacity explicitly and may
need multiple RF domains, gateways/backhaul, powered relay infrastructure,
channel/SF planning and controlled route/flood policy. This is a future network
milestone, not B4/M6 work.

## 9. Persistence, history and delivery truth

Important tracker records remain store-before-send. History, configuration,
security material, BLE bonds/DFU state and transient queues have different
ownership/reset/retention semantics and must not be collapsed into one untyped
store.

The current history region remains exclusively `0xED000..0xF4000`. The current
journal holds 728 compact position records, approximately 7.58 days at a 15-minute
report interval. The project goal of approximately 1–2 weeks is a **target**, not
a claim about current capacity.

Do not enable durable config before the flash/bootloader/SoftDevice/InternalFS/
bond/DFU ownership plan is verified. Do not remove the current SoftDevice flash
safety guard merely to make BLE writes succeed.

`TX_DONE` is local radio completion. It is not delivery, receiver custody,
authenticated contact, command execution, or confirmed physical state. Historical
replay/delivery cursors must not be advanced from TX completion without a defined
receipt semantic.

## 10. M6 LOST/security boundary

Local activity and local geofence development may proceed in M6 using accepted
fresh location and local rules.

A trustworthy network-contact-based LOST rule is different. Do not claim:

```text
OUTSIDE + no network contact for N hours -> trustworthy LOST
```

until "network contact" is backed by an appropriate authenticated receipt/contact
semantic. Existing v1 has no authentication/ACK contract and TX completion is not
contact evidence. Local OUTSIDE/NEAR state can exist before trustworthy remote
contact/delivery semantics.

Private person location, remote configuration, messaging and actuation remain
security-gated future work requiring authentication, authorization, anti-replay,
confidentiality where applicable, expiry/idempotency for commands and explicit
result/feedback semantics. Do not invent cryptography.

## 11. Ownership map

| Concern | Owner / allowed knowledge | Must not own/infer |
| --- | --- | --- |
| Device identity | identity provider + portable `DeviceIdentity` | radio readiness, user identity, profile |
| Hardware detection | board/sensor adapters + capability boundary | application role/profile |
| Capability state | capability snapshot: support/presence/health | requested user intent |
| Configuration | requested candidate + validation | driver probing, transport-specific policy |
| Resolution/effective state | combine validated request + capability/policy into status/reason | mutate requested intent silently |
| Profiles | defaults applied into requested config | immutable device classification |
| Application services | tracking/telemetry/activity/geofence/etc. | physical driver details, network topology inference |
| Location | source arbitration, validity, freshness, last-known state | u-blox parser internals, network role |
| Network | forwarding, dedupe, route/hop policy | sensor payload interpretation |
| Protocol codec | exact bytes and strict validation | radio ownership, business decisions |
| Radio transport | TX/RX ownership and callbacks; safely apply resolved forwarding state | sensor/application semantics, requested-config policy |
| Persistence | explicit region/format/retention owners | unallocated adjacent flash |
| Power policy/coordinator | explicit availability/energy policy and observable degradation | hidden rewriting of role/capability/user intent |
| `main.cpp` | composition root and cooperative orchestration | permanent accumulation of business rules |

Introduce a new abstraction only when a real dependency needs isolation. Do not
build a speculative generic HAL/event bus/plugin framework.

## 12. Current B2/B3 validation boundary

Code-bearing commit `a9d7bde7afcce2d7a34912c6fc4cdfcbf1788986`
has owner-run evidence for:

- full host regression suite PASS, including B1A/B2/B3 and M3/M4/M5/R2/R3/R4;
- B3 compatibility seam compiled under `gnu++11` to match the RAK toolchain;
- `pio run -e rak4630` SUCCESS;
- RAM 13,852 bytes (5.6%) and flash 139,576 bytes (17.1%) in that build;
- DFU upload SUCCESS to a RAK4631;
- runtime USB command path responding to `ROLE?` and `ROLE TRACKER` override.

Earlier B1B owner-operated hardware evidence demonstrated real open-sky GNSS on
Tracker B and DIRECT POSITION reception by Base A.

The owner/operator has now confirmed that the requested B2+B3 physical sanity
regression was also performed on the B2/B3 firmware: Tracker acquired a real GNSS
fix, produced/transmitted POSITION, and Base received the packet. Because the
current PositionFlow is store-before-send and suppresses live TX after append
failure, that successful packet path also exercises the normal store-first
admission path. This is not a substitute for separate flash power-cut/readback,
long-range RF, relay-path or current-consumption validation.

The short GNSS -> storage -> POSITION -> Base DIRECT regression gate is therefore
closed for B2/B3. The remaining closure gate is the independent final audit and
merge review. Host/build/upload evidence must still never be generalized into
unperformed hardware tests.

The 2026-09-15 independent architecture research review returned **GREEN WITH
CONDITIONS** and did not identify a P0/Critical architectural blocker. Accepted
pre-M6 refinements from that review are recorded here and summarized in
`docs/audits/PRE_M6_EXTERNAL_ARCHITECTURE_REVIEW.md`.

## 13. B4 bounded scope and current state

B4 implements only the minimum configuration/capability seam needed before M6:

- typed requested runtime configuration;
- pure whole-candidate validation;
- capability support/presence/health snapshot;
- requested -> effective resolution with explicit reason;
- legacy AUTO/USB compatibility source;
- independent forwarding runtime ownership;
- safe forwarding-state application through RadioManager;
- composition-root wiring from the compatibility request source into tracking
  and relay effective behavior.

Current B4 does **not** provide a user-facing independent relay toggle or durable
requested configuration. The architecture can represent TRACKER + relay ON and
the radio/network seams are host-tested for that combination, but production
requested intent still comes from the frozen legacy compatibility mapping until a
later explicit configuration surface is authorized.

The complete B4 branch review found one pre-merge semantic mismatch: an invalid
RequestedConfig could partially enable relay while blocking invalid tracking.
That was corrected before merge review so invalid candidates now fail closed as a
whole. The detailed review and accepted bounded limitations are recorded in
`docs/audits/PRE_M6_B4_BRANCH_REVIEW.md`.

B4 must **not** implement:

- durable config persistence;
- BLE;
- generic sensor registry/plugin framework;
- generic Location model without a second source;
- multi-hop/routing tables;
- TLP v2;
- security protocol;
- backend/mobile;
- actuation/commands.

B4 mixed-fleet direct hardware regression is physically observed: the B4 tracker
image on `0E8ADE7E71531AA3` produced a normal direct POSITION that the unchanged
legacy Base `09A462BD4B275BA5` received as sequence 4096. This closes only the
GNSS -> store-before-send -> frozen TLP v1 -> direct Base compatibility path for
the tested image. It does not prove relay coexistence, independently configured
TRACKER+relay hardware behavior, flash power-cut recovery, long-range RF, current
consumption or any future multi-hop behavior.

See `docs/milestones/PRE_M6_B4_CONFIG_CAPABILITY_BOUNDARY.md` for exact validation
state and remaining closure steps. Host/build evidence and the direct physical
PASS must not be generalized into unperformed hardware validation.
