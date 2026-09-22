# ORUN Current Architecture Rules

Status: **CURRENT against `main@699a74ea66cf3d22d9f1644fbf0f786f0092bd56` plus PR #33 follow-up evidence: M7P7E requester/response ownership remains merged on top of the M7P7D transport-neutral seam; M7P7B/C BLE runtime/application-boundary work and M7P6C/D/E security proof/pre-wire/coexistence work remain governing prerequisites. The corrected M7P6E fresh-pairing rerun is physically PASS for the scoped pinned RAK4631/framework/probe path; secure-envelope/provisioning/application-GATT runtime remains later and this result is not a claim of arbitrary CryptoCell thread-safety.**
Last reviewed against code checkpoint `main@699a74ea66cf3d22d9f1644fbf0f786f0092bd56` plus PR #33 code-bearing hardware-tested head `fb1b9408ae3c49cd3f6c26002a6d583aa18bb592`.
Last architecture review update: 2026-09-21 (§17 records the BLE application boundary and merged M7P7D/M7P7E request ownership seam).
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
return to role-owned forwarding. `main.cpp` resolves the legacy requested
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
Likewise, a RAK1904 accelerometer may be PRESENT while no activity service is
requested or effectively running.

The current M6A composition root marks the RAK product image as supporting both
GNSS and the owned RAK1904/LIS3DH path. `GnssManager` keeps the existing bounded
GNSS UNKNOWN/PRESENT/ABSENT detection semantics. `AccelerometerManager` adds an
independent bounded LIS3DH identity/configuration/sample seam: positive
`WHO_AM_I=0x33` establishes PRESENT ownership; clean bounded no/wrong-device
results become ABSENT; a transport/recovery fault before positive identity stays
UNKNOWN + FAULT; a failure after positive identity remains PRESENT + FAULT.
Neither capability may infer role, profile or service enablement.

GNSS acquisition-specific failures remain owned by the existing GNSS state
machine and do not make installed hardware disappear. Its current capability
projection therefore remains intentionally coarse: detected hardware is reported
as `PRESENT + OK`; acquisition timeout/recovery is not yet aggregated into the
capability health field.

The accelerometer performs the bounded boot/probe path and, only on explicit
`ACTIVITY START`, one bounded 50-sample diagnostic capture. M6B3 uses M6B1/M6B2
feature/quality helpers but is not an automatically enabled activity service.
Accelerometer presence must not imply activity enablement or classification.

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

The current production composition still derives RequestedConfig from the frozen
legacy role projection. This is intentionally only a migration source. Tracking
effective state gates PositionFlow/fix admission; relay effective state is
applied through the safe RadioManager forwarding boundary. M6A adds accelerometer
capability observation only; there is still no requested/effective activity or
geofence service field. A later validated configuration surface may replace the
legacy source without changing those ownership boundaries.

B4/M6's `RequestedConfig`/`CapabilitySnapshot`/`EffectiveConfig` model (tracking
enablement, relay forwarding, location source) remains runtime-only; do not
extend it into durable storage without its own reviewed migration. M7P5 added
one narrow, separately-scoped durable exception: `ConfigStore`
(`firmware/include/config_store.h`, `0x0E9000..0x0EB000`) persists exactly
`tracking_interval_seconds` and `battery_capacity_mah`. It does not persist or
migrate `tracking_enabled`, relay forwarding, role, location source, profile,
or capability, and does not replace or feed B4's requested/effective
resolution pipeline — `tracking_interval_seconds` only overrides the GNSS
schedule interval (`GnssManager::setTrackingIntervalMs`), nothing else.

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

B4/M6A do not change GNSS power ownership. `GnssManager::poll()` and the existing
sensor-rail state machine continue independently of application tracking service
resolution. Do not infer GNSS power from role or tracking enablement without a
separate reviewed power-policy change.

M6C1/M6C2 intentionally stop below this ownership boundary. Their `GeoPointE7`
value is geometry input only; the geofence geometry/area-set modules do not own
GNSS freshness, HDOP acceptance, source arbitration or last-known state. Future
runtime integration must hand the geofence service an already accepted fresh
position from the correct location/tracking owner rather than letting geometry
reach into `GnssManager` internals.

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

The current history region remains exclusively `0xED000..0xF4000`. The journal
holds 728 compact position records: approximately 7.58 days at the 15-minute
interval used through the M6P1 milestone report, but only approximately 1.52
days (~36.4 hours) at the current `main` development default of 3 minutes
(`firmware/include/gnss_config.h`, `kTrackingIntervalSeconds = 3 * 60`; see
`docs/architecture/ORUN_STORAGE_FLASH_OWNERSHIP.md` §13 for the arithmetic).
The project goal of approximately 1–2 weeks is a **target**, not a claim about
current capacity at either interval.

Do not remove the current SoftDevice flash safety guard merely to make BLE
writes succeed. See `docs/architecture/ORUN_STORAGE_FLASH_OWNERSHIP.md` for
the verified nRF52840 flash ownership map, the exact `InternalFS`-erases-history
mechanism, the SoftDevice-enabled blocker, and the required pre-M7/pre-store-forward
decision gates. `docs/architecture/ADR_M7_PERSISTENCE_LAYOUT.md` decides the partition plan:
`0x0E7000..0x0E9000` SecurityStore (M7P6B implementation head
`6d3009d42d9fb36026be5379171d8994a71dbaf6`; see `docs/milestones/M7P6B.md`),
`0x0E9000..0x0EB000` ConfigStore (M7P5), and
`0x0EB000..0x0ED000` relocated BLE bonds/InternalFS (M7P4), all strictly below
the unchanged HistoryStore region. SecurityStore owns credential + TX nonce-safety
state only and uses activation-last A/B recovery; ambiguous committed nonce state
fails protected TX closed. M7P7B is now merged on `main` at
`3b7eb0e6ae0275e6bf3e95f9c19f55c108cec87a` (PR #22): the production runtime
calls `Bluefruit.begin()` and keeps the M7P7A shared flash/event ownership model.
History, Config and Security async gate paths remain software/host-test validated;
the shared ConfigStore→FlashMutationGate→SoftDevice path additionally has real
hardware evidence under an active BLE connection (see below). M7P7A's single
physical-flash owner between the ORUN gate and relocated bond/InternalFS storage
remains the governing ownership boundary.

**M7P7B on main (merged via PR #22; `docs/milestones/M7P7B.md`):** first real
SoftDevice-enabled production BLE runtime with the minimal tracker admission policy
(~10-min no-client window, connected suspends it, disconnect grants one fresh
window, one client). It adds no ORUN application GATT service, ORUN pairing/ownership,
provisioning or authorization; stock Bluefruit pairing/bonding is reachable and a
framework bond is **not** ORUN authorization. Physical evidence on one RAK4631 with
a Samsung/nRF Connect client now covers: real advertising/scan/connect; connected
past the no-client deadline; the §8.5 direct-event disconnect → loop-owned restart
→ fresh window → reconnect lifecycle; no-client close and clean cold boot; stock
bond creation plus power-cycle persistence/reconnect; BLE coexistence with real
LoRa direct RX and BLE-connected RELAY RX/QUEUE/TX/TX_DONE; and a real
ConfigStore→FlashMutationGate→SoftDevice async flash mutation while BLE stayed
connected (6/6 accepted completions, zero errors/timeouts/late completions/
disconnects, temporary config verified and exact original restored). That flash
probe physically validates the shared ConfigStore/gate/event-bridge path but does
not separately claim HistoryStore or SecurityStore client-specific mutation; LoRa
`TX_DONE` remains local radio completion, not delivery. Quantitative current/power remains unmeasured and is explicitly
**deferred, not PASS**, because no measurement equipment is available; the owner
accepts that gap as non-blocking for M7P7B. GNSS coexistence remains **blocked on
this unit** (`GNSS: not detected`) and is owner-waived as a blocker for this
milestone merge only; later physical verification on a GNSS-equipped unit is
still required. The deliberately between-loop-polls lifecycle timing and
advertising start/stop failure injection remain host-only and are not physical
PASS. Secure envelope, provisioning, application GATT, DFU and LoRa `OPEN_BLE`
remain later work.

M6 activity/geofence helpers allocate no durable state and do not reuse the
position journal. Future activity history, polygon configuration, FREE_GRAZE
state or critical events require explicit storage owners and power-cut semantics
before persistence is enabled.

`TX_DONE` is local radio completion. It is not delivery, receiver custody,
authenticated contact, command execution, or confirmed physical state. Historical
replay/delivery cursors must not be advanced from TX completion without a defined
receipt semantic.

## 10. M6 LOST/security boundary

Local activity and local geofence development may proceed in M6 using accepted
fresh location and local rules.

The current software stack has bounded explicit M6B3 diagnostic activity
capture plus deterministic activity feature/quality helpers and geofence
geometry/permitted-area composition. It does **not** yet implement an automatically
enabled production activity service or production activity classification,
NEAR_FENCE distance, GNSS quality policy, repeated-fix confirmation, hysteresis,
FREE_GRAZE or local operational state. Host-only geometry PASS must not be described as field geofence PASS.

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
| Accelerometer manager | LIS3DH identity/config/sample + bounded shutdown | activity enablement/classification, role, GNSS power |
| Capability state | capability snapshot: support/presence/health | requested user intent |
| Configuration | requested candidate + validation | driver probing, transport-specific policy |
| Resolution/effective state | combine validated request + capability/policy into status/reason | mutate requested intent silently |
| Profiles | defaults applied into requested config | immutable device classification |
| Activity window/quality | deterministic local features + eligibility | sensor I/O, cattle accuracy claims, RF/storage policy |
| Geofence geometry/area-set | polygon validity, point relation, permitted union | GNSS freshness/source, NEAR/hysteresis/FREE_GRAZE/LOST |
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

The owner/operator confirmed the requested B2+B3 physical sanity regression on
the B2/B3 firmware: Tracker acquired a real GNSS fix, produced/transmitted
POSITION, and Base received the packet. Because PositionFlow is store-before-send
and suppresses live TX after append failure, that successful packet path also
exercises the normal store-first admission path. This is not a substitute for
separate flash power-cut/readback, long-range RF, relay-path or current-consumption
validation.

The short GNSS -> storage -> POSITION -> Base DIRECT regression gate is therefore
closed for B2/B3. Host/build/upload evidence must still never be generalized into
unperformed hardware tests.

The 2026-09-15 independent architecture research review returned **GREEN WITH
CONDITIONS** and did not identify a P0/Critical architectural blocker. Accepted
pre-M6 refinements from that review are recorded in
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

The B4 branch review found one pre-merge semantic mismatch: an invalid
RequestedConfig could partially enable relay while blocking invalid tracking.
That was corrected so invalid candidates fail closed as a whole. The later Astra
review also found undefined `RequestedLocationSource` enum values were outside
the validation domain; that was corrected and covered by regression tests before
the B2/B3/B4 stack was merged.

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
state. Host/build evidence and the direct physical PASS must not be generalized
into unperformed hardware validation.

## 14. Current M6 implementation boundary

M6A and M6B3 are runtime-integrated. M6B3 explicitly connects the portable
M6B1/M6B2 helpers for one manual diagnostic capture. M6C1/M6C2 remain host-only.

### Runtime-integrated now

- `AccelerometerManager` performs bounded RAK1904/LIS3DH boot detection,
  cooperative configuration, retained-sample discard, one fresh probe sample and
  bounded post-sample shutdown.
- `CapabilitySnapshot` includes independent accelerometer support/presence/health
  alongside GNSS.
- This capability observation does not create an activity requested/effective
  service and does not affect Role, tracking, relay forwarding or GNSS power.

Historical M6A software and final independent Astra audit are PASS on
`332cf0e1b307735348a97c3cbd15f916d04a21a0`: **13,948 / 248,832 bytes RAM (5.6%)**
and **142,456 / 815,104 bytes flash (17.5%)**. The diagnostic delta versus
`613cdf1` adds **16 bytes RAM / 272 bytes flash**. `ACCEL?` only reports the latched
boot result; it does not probe, wake, reconfigure or re-enter the manager, and
changes no TLP v1, RF, storage, GNSS, identity, sequence, role compatibility or
power ownership semantics.

The focused operator M6A physical gate is **PASS / CLOSED** on that exact image:
upload to Tracker B, ABSENT with no module, positive RAK1904 identity and normal
settled/fresh XYZ with the module in SENSOR C, and automatic PRESENT on a separate
reset without `ACCEL?`. Only this narrow focused probe path is physically proven.
`ACCEL PRESENT` supports the normal shutdown-write path because the current
manager emits `kPresent` only after successful post-sample `powerDownSensor`
completion. This is not a current-consumption measurement and does not prove
physical I2C fault-cleanup/recovery. This is operator evidence, not independent
Astra hardware validation. Exact serial observations and the PASS/non-evidence
matrix are recorded in `docs/milestones/M6.md` and
`docs/audits/PRE_M6_STACK_AUDIT_RESOLUTION.md`.

### M6B3 explicit diagnostic capture

`AccelerometerManager` remains the sole LIS3DH I2C/configuration/sample owner.
A runtime session reuses M6A's power-down-first configuration, 700 ms HR settling,
retained-sample discard and shutdown/fault cleanup. One pending sample is handed
to `ActivityCapture` without overwrite. The coordinator collects exactly 50,
assesses features with `assessActivityWindow()`, and exposes READY/INVALID only
after successful shutdown. Sensor faults suppress the result and retain
`PRESENT + FAULT`, including after sparse cleanup recovery. `ACTIVITY?` is query-only.

There is no boot/periodic auto-start, RequestedConfig activity field, role-based
enablement, classification, RF activity telemetry or persistent activity history.
This does not freeze a production duty cycle, FIFO or interrupt policy. Separate
operator evidence on the exact M6B3 image closes the focused bounded diagnostic
capture physical gate: boot regression, 50-sample usable features, latched query,
BUSY rejection without losing capture, restart with new features and sensorless
ABSENT rejection. READY supports the implemented confirmed shutdown-write path;
it is not a current measurement or physical fault-cleanup validation. This is not
independent hardware validation. See `docs/milestones/M6B3.md` for exact records.

Observed duration was 5790 ms across 49 intervals: about 118 ms / 8.5 Hz effective
average, within the existing timing-quality contract (zero discontinuities).
Nominal 10 Hz / approximately 5 seconds remains an implementation seed, not an
exact physical sampling claim. Field data and feature sensitivity to sample-rate
variation must inform future classifier thresholds/models. No timing policy changes.

### Host-only / not production-integrated now

- M6C1 simple-polygon geometry;
- M6C2 multiple permitted-area union composition.

Each of those slices has owner-run full host regression PASS and RAK4630 build
SUCCESS on the stacked branch. The geofence helpers remain linker-removed
because they are not referenced by runtime. M6B3 now links the activity helpers;
its footprint is recorded separately in `docs/milestones/M6B3.md`.

No current M6 code claims:

- continuous production accelerometer sampling;
- RESTING/GRAZING/WALKING classification;
- cattle behavior accuracy;
- GNSS-to-geofence runtime wiring;
- NEAR_FENCE distance, hysteresis or repeated-fix policy;
- FREE_GRAZE;
- geofence persistence;
- critical RF event/ACK delivery;
- trustworthy network-contact LOST.

The prior final independent audit and focused operator M6A physical gate are
closed for the recorded M6A image. Separate current-image operator evidence now
closes the M6B3 bounded diagnostic physical gate. Automatic activity enablement,
classification, M6C runtime and M6D remain outside this closure.
Overall M6 remains IN PROGRESS; no
continuous sampling, animal classification/accuracy, geofence field behavior or
trusted LOST/contact is validated by this closure. Any later change to
runtime/I2C/power behavior requires relevant revalidation before merge.


## 15. M7P6 security direction and current durable implementation

The authoritative design record is
`docs/architecture/ADR_M7P6_SECURITY_ARCHITECTURE.md`; implementation evidence is
`docs/milestones/M7P6B.md`.

- `DeviceIdentity` remains a stable public lookup/compatibility identity, not an
  authenticator.
- M7P6B implements one independent credential lifetime per device:
  128-bit random `credential_id`, `key_epoch`, 256-bit `K_root`, plus separate
  TX nonce/counter reservation state.
- No fleet/group authentication authority key is used.
- SecurityStore owns only credential + TX nonce-safety state. User IDs, owner IDs,
  phones and detailed permission tables remain backend/app concerns.
- TX security counter state is completely separate from TLP v1 sequence/history tickets.
  Reservation block size is 256 and the durable value is an absolute exclusive bound.
- After reboot, unused counters from the previous reserved block are skipped; a fresh
  durable reservation is required before another protected TX counter may be returned.
- SecurityStore uses two raw 4 KiB A/B pages at `0x0E7000..0x0E9000`. The page-header
  commit word is the final page activation marker after the complete credential/counter
  snapshot is durable.
- Unsupported future format, ambiguous committed corruption, invalid non-erased
  reservation records and impossible append gaps fail protected security state closed
  rather than falling back to a lower nonce bound.
- Re-provisioning creates a new security lifetime; current `credential_id` or current
  root reuse is refused when resetting the TX counter.
- Flash admission is `SEC_CRITICAL > History > Config > SEC_MAINT`, with bounded
  anti-starvation aging. Bond/InternalFS is not yet part of that gate.
- A normal relay/gateway remains an opaque transport/custody participant and does not
  gain tracker root keys or trusted ACK/contact authority merely by being a gateway.
- RX replay HWM, command IDs/results and device-side authorization/delegation are later
  secure-downlink/command-layer state, not SecurityStore v1 fields.
- TLP v1 bytes remain frozen and unauthenticated. Future trusted traffic must use an
  explicit secure envelope/version; M7P6B adds no secure RF bytes.
- Standards-based crypto only. HKDF-SHA256 + compact standard AEAD remains the current
  later-envelope direction, but exact library, nonce/AAD/header/tag bytes are not frozen.
- No production provisioning transport exists. Blank devices remain UNPROVISIONED and
  current TLP v1 operation continues.
- Real RAK4630/RAK4631 bootloader authenticity/rollback behavior, partition preservation
  across actual DFU/update paths, electrical power-cut and SoftDevice-enabled async
  security flash remain physically unverified.

Milestone sequence remains: M7P6A design -> M7P6B durable foundation ->
later secure envelope -> later authenticated commands/authorization. BLE commissioning
and diagnostic transport remain later BLE work; do not confuse BLE bonding with
application authorization.

## 16. Shared RF domain, coverage learning and diagnostics direction

The authoritative product/design record is
`docs/architecture/ORUN_FIELD_NETWORK_DIAGNOSTICS_PLAN.md`.

RF channel/domain is not customer/project identity. Infrastructure placed in one pasture
or customer area should be able to help another eligible ORUN node in RF range when the
shared RF plan and forwarding policy permit it. Customer/project separation belongs to
security/backend ownership, not to a permanently dedicated channel.

The long-term planning model may use overlapping RF cells/spatial reuse: not every node
must hear every other node, but do not deliberately create connectivity dead zones merely
to raise capacity. Channel/domain splits are a measured capacity/interference tool, not
today's default scaling mechanism. A future multi-channel concentrator-class gateway is
an infrastructure option only when real load requires it; do not add speculative channel
hopping, SX130x HALs or per-customer RF allocation now. Current frozen TLP v1 still
supports exactly one relay hop and rejects nested RELAY_FORWARD.

Coverage learning remains a product requirement. Backend/app should eventually combine
accepted device position/time with actual reception observations (receiving gateway/relay,
DIRECT vs RELAY path, RSSI, SNR, time and RF configuration) to build empirical coverage
maps and improve relay/gateway placement. RSSI/SNR are not metres, and absence of an
observation is not proof of no coverage; unknown/insufficient-data must remain distinct
from repeatedly observed weak/dead areas. Do not turn tracker flash into a long-term
coverage database.

Diagnostics should have one bounded transport-neutral Health/Diagnostics owner. Current
USB Serial already exposes useful reset/storage/radio/activity/RSSI/SNR/path/error
evidence. Future BLE should expose structured status snapshots, counters and a small
recent-event view through the same owner rather than mirror an unlimited Serial stream.
Tracker BLE remains normally OFF. Whenever BLE is open and no client is connected, an
approximately 10-minute no-client timeout applies; expiry closes BLE. A connected client
suspends that no-client timeout. On disconnect, a fresh approximately 10-minute no-client
timeout starts; if nobody reconnects, BLE closes. A separate stalled-session watchdog is
still required for a client that stays connected without making progress. Gateway profiles
may keep BLE available when their power/availability contract permits it. Diagnostic data
may be sensitive, so BLE connection/bonding alone must not imply authorization.

Normal application UI should show useful health/coverage outcomes; detailed user/account
permissions remain backend-owned, and the app should hide unauthorized controls. Device
firmware still independently verifies cryptographic authority for protected operations.

Internet backhaul is not a prerequisite for on-site local ORUN operation. When an
authorized user is physically at the site, a compatible ORUN gateway should eventually
bridge locally available/cached accepted location state and later offline-capable MESSAGE
plus authorized configuration/key/access/control traffic over an implemented local
transport such as BLE or wired/USB, without requiring cloud reachability.

Owner-approved product direction includes **targeted remote configuration through a
gateway**. The gateway is a transport/bridge, not the configuration owner or security
authority: it must not invent authority, rewrite target intent, or directly mutate a
tracker's durable configuration. The target device must authenticate and authorize the
operation, enforce anti-replay plus freshness/expiry and idempotency, validate the
candidate configuration, and apply accepted changes through the same application/config
owner used by direct local transports (currently the ApplicationRequestService/ConfigStore
boundary). Gateway receipt, RF TX completion and target receipt are not configuration
success; UI/backend may report success only from an explicit target-device result that the
requested change was accepted/applied. A local offline path such as
phone -> BLE -> gateway -> LoRa -> target device must remain possible without Internet.

Service security remains application-specific: private messages keep end-to-end
protection, and configuration/key/access/control operations still require authentication,
authorization, anti-replay, freshness/expiry, idempotency and explicit result semantics.
This does not make the gateway a security authority or give it tracker root keys. Current
TLP v1 bytes remain frozen; future trusted remote configuration/command traffic requires
an explicitly versioned secure application envelope rather than reinterpretation of TLP
v1. "Any gateway" means any compatible gateway that actually holds or can locally reach
the requested traffic/state; complete-site visibility through one arbitrary gateway while
offline would require an explicit local cross-gateway synchronization design and is not
implemented today. See `ORUN_FIELD_NETWORK_DIAGNOSTICS_PLAN.md`.

## 17. BLE application boundary and later application direction

M7P7B makes BLE transport available; it does not make a connected or bonded phone
an authorized ORUN application client. Future application GATT remains a transport
adapter into existing application/configuration/command owners rather than a second
business-logic or configuration system. BLE callbacks perform bounded handoff;
flash, crypto, radio transitions and application execution remain owned by reviewed
loop/task code. See `docs/milestones/M7P7C.md`.

The exact commissioning ceremony remains a later focused implementation decision.
Connection, stock BLE bonding, device credential, user identity and application
authorization remain separate. No generic `K_root` readback or transport-triggered
credential export is authorized. Production provisioning must also define authority key custody/recovery and
respect the scheduling/ownership implications exposed by the scoped
fresh-pairing CC310/Bluefruit coexistence PASS. That scoped test closes the
pinned-path evidence gate only; it does not establish arbitrary CryptoCell
thread-safety or pre-decide that a backend stores raw `K_root`.

Future Entity Registry, optional person-location, shared-map and MESSAGE
routing/delivery/offline-sync decisions are application architecture rather than BLE
milestone invariants. Their current owner-approved direction is recorded in
`ORUN_APP_ENTITY_MESSAGING_DIRECTION.md`. That record is documentation-only and
does not claim backend, Android, MESSAGE, secure-RF or offline-sync runtime exists.

M7P7D is the first implementation step under this boundary: a fixed-memory,
loop-owned, transport-neutral application request/result seam with a read-only USB
`APP CONFIG?` adapter that reads through the existing ConfigStore owner. It does
not define BLE wire bytes, expose protected writes, or change authorization/security
semantics. See `docs/milestones/M7P7D.md`.

M7P7E (PR #34, merged at
`main@cb1e181f88ed8d8362f6d4d2f97b96734474c954`) refines that internal seam
before a second adapter exists. Accepted application work is tagged with an
explicit local requester; unsupported requester values fail closed before they
can own the global response slot. A pending result is consumable or discardable
only by its requester, while the single global slot continues to provide bounded
BUSY backpressure. Numeric request IDs are local to the requester namespace.
Requester provenance is not user identity, authorization, BLE connection
identity or a wire field. This remains internal ownership only: it adds no BLE
GATT, wire format, authorization, provisioning, storage or RF behavior. See
`docs/milestones/M7P7E.md`.

M7P7F (branch `feat/m7p7f-ble-app-transport-contract`, not yet merged) freezes
the exact bounded GATT transport contract that M7P7E's "next gate" section
anticipated: three 128-bit UUIDs, an 8-byte header / 20-byte-frame /
48-byte-logical-payload / 4-fragment wire rule, `GET_CONFIG` request/response
and `ERROR` byte layouts, and a portable, Bluefruit-free
`BleApplicationTransport` that implements the session-hygiene requirements
above (generation-gated `endSession()`, prompt release of the global
`ApplicationRequestService` slot, stop-and-wait backpressure, a bounded
2-second fragment-reassembly timeout). It adds no Bluefruit
`BLEService`/`BLECharacteristic`, no change to BLE admission/advertising/bond
behavior, and is not referenced by production `main.cpp` composition. See
`docs/milestones/M7P7F.md`. Wiring this contract to real Bluefruit
callbacks/indications and physical phone validation remain M7P7G.

