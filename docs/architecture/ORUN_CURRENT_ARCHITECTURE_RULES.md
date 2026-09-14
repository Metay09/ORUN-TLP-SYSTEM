# ORUN Current Architecture Rules

Status: **CURRENT pre-M6 owner-approved architecture rules**.
Last reviewed against code: `a9d7bde7afcce2d7a34912c6fc4cdfcbf1788986`.
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
- hardware presence;
- requested configuration;
- effective runtime state;
- system power policy.

A profile is only a versioned preset/default bundle. It is not a permanent
hardware restriction or topology identity.

## 2. One firmware, configuration decides behavior

ORUN uses one firmware codebase on the current RAK4630/RAK4631 reference
platform. We do not build separate tracker, relay, gateway or sensor firmware
projects.

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
TX and resumes afterwards. Future power policy must not silently switch a
user-enabled relay off; incompatibility must be rejected or made observable.

Gateway bridging is separate from LoRa relay forwarding. A gateway can bridge
without repeating RF traffic, relay without gateway service, or do both.

## 4. Current legacy compatibility remains frozen

The current B3 compatibility projection intentionally preserves M5 behavior:

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

## 5. Capability model and product visibility

Keep these facts separate:

```text
firmware supports a hardware family
!= hardware is physically present/assigned
!= service is requested
!= service is effectively running
```

Optional digital hardware should be detected with a bounded probe that validates
an identifying/product/protocol response when practical. An I2C address alone is
not sufficient proof when multiple device families can share it. SPI, UART and
1-Wire devices use their equivalent product/family/protocol identity when the
component provides one.

A generic analog input, dry contact or other self-describing-impossible sensor
cannot tell the MCU what physical quantity it represents. Its channel therefore
requires an explicit configured assignment when that feature is implemented.

Normal user-facing UI rule:

```text
currently present/assigned capability -> show it
currently absent capability           -> hide it
```

This keeps a universal firmware image from filling the UI with sensors that do
not exist on that device. Bounded diagnostics may record detection state and
failures without inventing presence in normal product UI.

Capability does not imply service enablement. A GNSS module may be present while
GNSS tracking is disabled or a different location source owns the active point.

## 6. Location and GNSS remain separate

GNSS is one hardware/source implementation. Location is the higher-level data
and ownership concept.

- GNSS presence does not imply TRACKER/profile selection in the target model.
- GNSS power state does not imply location validity.
- A missing GNSS device must not turn a provisioned node into BASE/gateway/relay.
- A source disappearing must not erase the last valid point.
- `0,0` is a valid coordinate, never a missing-value sentinel.
- Recovered/persisted data is not automatically live/fresh.

B2 already moved the portable GNSS value to `GnssFix` and the frozen legacy
POSITION mapping to a pure boundary. Future location-source work must preserve
legacy v1 bytes and GNSS freshness semantics.

## 7. Network evolution boundary

Current TLP v1 behavior is frozen:

- POSITION is the existing 34-byte v1 packet;
- RELAY_FORWARD is the existing 49-byte v1 one-hop wrapper;
- current RELAY rejects nested relay envelopes;
- current dedupe/queue limits remain as tested;
- TLP v1 bytes and golden compatibility fixtures must not be weakened.

The one-hop rule is a **current v1 compatibility rule**, not a permanent ORUN
architecture limit. Future ORUN may support controlled bounded multi-hop after a
separate network/protocol design and validation milestone. That work must define
stable network message identity, duplicate suppression, hop/flood bounds, airtime
admission, security/authentication, mixed-fleet behavior, reset/cache behavior
and field tests. Do not introduce unlimited flooding or an accidental mesh by
simply forwarding relay envelopes again.

## 8. Ownership map

| Concern | Owner / allowed knowledge | Must not own/infer |
| --- | --- | --- |
| Device identity | identity provider + portable `DeviceIdentity` | radio readiness, user identity, profile |
| Hardware detection | board/sensor adapters + capability boundary | application role/profile |
| Configuration | validated config owner | driver probing, transport-specific policy |
| Profiles | defaults applied into config | immutable device classification |
| Application services | tracking/telemetry/activity/geofence/etc. | physical driver details, network topology inference |
| Location | source arbitration, validity, freshness, last-known state | u-blox parser internals, network role |
| Network | forwarding, dedupe, route/hop policy | sensor payload interpretation |
| Protocol codec | exact bytes and strict validation | radio ownership, business decisions |
| Radio transport | TX/RX ownership and callbacks | sensor/application semantics |
| Persistence | explicit region/format/retention owners | unallocated adjacent flash |
| Power coordinator | resource/availability policy | hidden rewriting of role/capability/user intent |
| `main.cpp` | composition root and cooperative orchestration | permanent accumulation of business rules |

Introduce a new abstraction only when a real dependency needs isolation. Do not
build a speculative generic HAL/event bus/plugin framework.

## 9. Current B2/B3 validation boundary

Code-bearing commit `a9d7bde7afcce2d7a34912c6fc4cdfcbf1788986`
has owner-run evidence for:

- full host regression suite PASS, including B1A/B2/B3 and M3/M4/M5/R2/R3/R4;
- B3 compatibility seam compiled under `gnu++11` to match the RAK toolchain;
- `pio run -e rak4630` SUCCESS;
- RAM 13,852 bytes (5.6%) and flash 139,576 bytes (17.1%) in that build;
- DFU upload SUCCESS to a RAK4631;
- runtime USB command path responding to `ROLE?` and `ROLE TRACKER` override.

Earlier B1B owner-operated hardware evidence demonstrated real open-sky GNSS on
Tracker B and DIRECT POSITION reception by Base A. That prior hardware evidence
is the regression reference; it is not automatically proof that every later
commit has repeated the same physical chain.

The exact B2+B3 commit still requires its final independent audit/merge gate and,
when hardware access permits, a short regression of the previously proven
GNSS -> storage -> POSITION -> Base DIRECT path. Do not label host/build/upload
as that physical RF/GNSS PASS.

## 10. Documentation maintenance rule

For every meaningful change, review the affected architecture, protocol,
ownership, milestone and compatibility documents. Update only documents whose
truth changed; do not create documentation churn for unrelated edits.

Before merge, the change report must explicitly state:

- code/runtime impact;
- architecture/ownership impact;
- TLP wire compatibility impact;
- persistence/flash impact;
- RF/airtime and power impact;
- documentation updated;
- host/build/sanitizer status;
- physical hardware status and what remains untested.
