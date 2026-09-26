# ORUN Product / System Architecture Direction

Status: **OWNER-APPROVED PRODUCT / SYSTEM ARCHITECTURE DIRECTION — 2026-09-26.**

Baseline: `main@e2a370510c595c5f4b88e94a1212fb95d848a273`.

This document is the high-level architecture map for ORUN. It does not replace
focused ADRs, protocol documents, milestone evidence or compatibility fixtures.
It defines the product decomposition and ownership boundaries that future slices
must preserve.

The purpose is to keep ORUN understandable as one system while avoiding two
opposite failures:

1. coupling every feature to today's RAK4631 / SX1262 implementation; or
2. building speculative frameworks, HALs and abstractions for hardware or scale
   that does not yet exist.

The rule is:

> Future fruit determines the shape of the trunk, but only today's required
> branch is implemented.

---

## 1. Root — product outcomes

The architecture exists to produce reliable user-visible answers, not merely
packets or passing tests.

The primary product outcomes are:

- Where is the tracked animal/device/entity?
- When was that position actually observed?
- Is the entity moving, stationary, grazing or otherwise active?
- Is the observation fresh, stale, historical or last-known?
- Is battery/device health acceptable?
- Is the entity inside, near or outside an allowed area?
- Is there an active LOST/critical alarm?
- Was important data retained while RF/phone/Internet was unavailable?
- Was a command merely transmitted, or actually authenticated and applied?
- Was a message delivered to the intended endpoint?
- What path carried the data: direct, relay, gateway, local BLE/USB or backend?
- Can an authorized local user still operate when Internet is unavailable?

Livestock tracking is the first serious product use case. The architecture must
also remain suitable for future low-power field telemetry, sensing, messaging
and controlled actuation without turning today's firmware into a speculative
general-purpose framework.

---

## 2. Engineering scale target

ORUN is **not** currently targeting a flat 1000-device RF domain.

Use these planning levels:

- **~10 devices:** basic field-network baseline;
- **~30-50 devices:** meaningful medium-load validation;
- **~100 devices in one RF domain:** current upper engineering stress target,
  not a guaranteed commercial capacity number;
- **1000 devices:** only a future architecture sanity question. If such scale
  ever becomes real, it must use explicit RF/domain/gateway partitioning rather
  than assuming one SF11 channel or one gateway domain carries a flat fleet.

The purpose of the upper target is to expose hidden O(N), queue, airtime,
persistence and recovery defects early.

Capacity is never defined by node count alone. It depends on:

- packet airtime and data rate;
- reporting cadence;
- relay retransmissions;
- alarm/event bursts;
- retries;
- command/result traffic;
- history/backlog replay;
- collision domain and gateway placement;
- regional duty-cycle/install policy.

A design that works at 10 nodes but collapses at 50-100 under realistic traffic
is not considered robust.

---

## 3. Trunk — system layers

ORUN is divided into five architectural layers.

### 3.1 Device foundation

Cross-cutting device state and ownership:

- Device Identity
- Capability support / presence / health
- Requested configuration
- Effective/applied configuration
- Profile/defaults
- Security authority and credentials
- Replay / nonce state
- Persistent storage ownership
- Power policy
- Time/freshness quality
- Firmware lifecycle / DFU
- Diagnostics/health
- Hardware-driver ownership

These are foundations, not application features.

### 3.2 Application services

Independent product services:

- Location
- Tracking
- Telemetry
- Sensor observations
- Activity
- Geofence / LOST
- Event / Alarm
- Messaging
- Command / Result
- Health / Diagnostics
- History / Store-forward

A service may use several transports. A transport must not become the owner of
service truth.

### 3.3 Network / transport

Transport responsibilities:

- LoRa P2P RF
- Relay forwarding / custody
- Gateway bridging
- BLE
- USB / wired local service
- Future IP/backend transport

Transport answers "how did bytes move?", not "what does the application state
mean?".

### 3.4 Edge / gateway

A gateway may combine several independent responsibilities:

- LoRa endpoint;
- local bridge to phone/USB/IP;
- opaque store-forward custody;
- backend synchronization;
- diagnostics aggregation;
- explicitly enrolled delegated command authority when approved.

Gateway bridge capability is not automatically relay forwarding, user
authorization, tracker identity or root security authority.

### 3.5 App / backend

Backend/application responsibilities include:

- users and permissions;
- Entity Registry;
- long-term location/history;
- telemetry/event storage;
- maps and visualization;
- alarms/notifications;
- command lifecycle;
- message routing/custody;
- gateway enrollment;
- security authority services;
- fleet/site configuration policy;
- coverage/capacity analytics.

The backend is not allowed to overwrite device observation time with backend
receipt time and then present old data as live.

---

## 4. Non-negotiable concept separation

Keep these independent:

```text
Role
!= Location Source
!= GNSS Power
!= Capability
!= Enabled Service
!= Transport
!= Device Identity
!= Profile
!= User Identity
!= Security Authority
```

Also keep separate:

```text
Replay
!= Freshness
!= Nonce safety
!= Idempotency
!= Delivery
!= RESULT
```

And:

```text
Requested config
!= Capability
!= Effective config
!= Applied hardware state
```

A future implementation that collapses these concepts needs an explicit
architecture review before merge.

---

## 5. Branches — service ownership

### 5.1 Location

Location is a source-neutral product fact with:

- value;
- source;
- observation time;
- validity;
- freshness/age;
- quality;
- last-known semantics.

GNSS is one possible source, not the definition of Location.

Future sources may include phone, manual/fixed, network-assisted or another
reviewed source. Do not force non-GNSS location into GNSS-specific structures.

### 5.2 Tracking

Tracking owns policy such as:

- when a fresh location is requested;
- movement-dependent reporting cadence;
- stationary cadence;
- history generation;
- critical state escalation.

Tracking consumes Location and power/network policy. It does not own the GNSS
driver.

### 5.3 Telemetry

Telemetry represents periodic/observational numeric or bounded state values,
for example:

- battery voltage/estimate;
- temperature;
- humidity;
- pressure;
- device current;
- signal/link summary;
- future sensor metrics.

Telemetry is not an alarm merely because one value crosses a threshold.

### 5.4 Sensor / Activity

Raw sensor drivers own hardware sampling.

Higher services own interpretation:

```text
driver sample
-> bounded feature extraction
-> activity/sensor observation
-> optional event/alarm decision
```

Raw high-rate data is not continuously sent over LoRa.

### 5.5 Event / Alarm

An EVENT is a stable occurrence, not a telemetry flag.

Examples:

- geofence OUTSIDE;
- LOST;
- critical battery;
- sensor fault;
- tamper;
- future safety event.

An alarm lifecycle needs occurrence identity, state, severity, observation time
and clear/update semantics.

Critical event traffic has higher QoS than routine history or messaging.

### 5.6 Geofence / LOST

Geofence evaluation remains local where safety/value requires offline behavior.

It consumes accepted Location, not GNSS internals.

LOST is an application/network state machine, not a radio RSSI threshold.

### 5.7 Messaging

MESSAGE is transport-independent application data.

It owns:

- stable `message_id`;
- sender/recipient semantics;
- delivery state;
- bounded TTL/store-forward;
- confidentiality policy.

Message identity is separate from security nonce/counter and transport dedupe.

Private MESSAGE may require end-to-end protection so relay/gateway/backend
custody does not imply plaintext access.

### 5.8 Command / Result

COMMAND owns an intended operation.

RESULT owns authenticated application outcome.

`TX_DONE`, relay custody or gateway receipt never means `APPLIED`.

Side-effecting commands require:

- authorization;
- replay protection;
- freshness/precondition;
- idempotency/CAS;
- durable application ownership;
- authenticated RESULT.

### 5.9 Health / Diagnostics

Health is a bounded service model, not raw serial logging.

It may expose:

- subsystem state/reason;
- counters;
- reset/recovery facts;
- bounded recent important transitions.

USB/BLE/backend are consumers of this model.

### 5.10 History / Store-forward

History persists important observations before transmission where required.

Priority order remains conceptually:

1. critical events;
2. current/live data;
3. historical position;
4. historical activity/telemetry.

Backlog must not starve current critical data.

---

## 6. Entity Registry and map model

The backend Entity Registry is the long-term owner of real-world assignment and
presentation metadata.

An entity may represent:

- animal;
- person;
- vehicle;
- gateway;
- sensor installation;
- future actuator/valve or other field asset.

Entity category/icon/name is application metadata, not firmware role or
capability.

The shared map is a view over:

```text
Entity Registry
+ accepted Location
+ freshness/age
+ alarms/events
+ selected health state
```

Map rendering must distinguish:

- live/fresh;
- stale/last-known;
- historical;
- unknown.

Offline map tiles are a mobile application concern. Coordinates and local entity
state can remain available without Internet.

---

## 7. End-to-end data flows

### 7.1 Observation path

```text
physical sensor/source
-> driver observation
-> validation/quality
-> application service
-> local persistence if required
-> transport admission
-> LoRa/BLE/USB/IP
-> gateway/backend
-> durable backend model
-> app/map/notification
```

Each stage keeps provenance and time semantics.

### 7.2 Command path

```text
authorized user/policy
-> command intent
-> authenticated authority
-> protected COMMAND
-> optional opaque custody/store-forward
-> target replay/freshness/idempotency checks
-> application owner
-> durable effect
-> authenticated RESULT
-> user-visible state
```

### 7.3 Message path

```text
sender
-> stable MESSAGE identity
-> route/custody selection
-> Internet or local gateway/LoRa fallback
-> endpoint decrypt/validate/store
-> delivery state
```

The same logical message keeps one application identity across route changes.

---

## 8. RF/network architecture

Current reference behavior remains private LoRa P2P.

Current validated TLP v1 one-hop behavior remains frozen until an explicit
protocol cutover.

Near-term network principles:

- tracker power is prioritized;
- relay/gateway availability is explicit;
- relay forwarding is a service commitment, not a Role side effect;
- critical alarm/result traffic gets priority over backlog/chat;
- airtime is a budget;
- collision-domain load matters more than total fleet count;
- adaptive reporting is preferable to blindly shortening every tracker interval.

If future scale exceeds one practical RF domain, consider in this order:

1. field evidence and gateway placement;
2. adaptive cadence/data-rate policy;
3. additional gateways;
4. spatially separated RF domains;
5. additional channels/domains when justified;
6. concentrator-class infrastructure only when measured requirements justify it.

Do not build multi-channel/concentrator support before a real need and hardware
selection exist.

---

## 9. Security architecture

Security authority is independent from transport.

Current rules:

- TLP v1 remains unauthenticated legacy/development traffic;
- protected commands/messages/private location require reviewed TLP v2 security;
- no fleet-wide/group authentication secret for command authority;
- no custom cryptography;
- gateway enrollment and user authorization remain separate;
- BLE bonding/PIN is not ORUN application authorization;
- replay state mutates only after authentication;
- protected application action occurs only after required durable replay state;
- reset/rollback must never resurrect an accepted command;
- stale raw security-store restore requires fail-closed/re-provision policy.

M7P6F remains the current durable backend/asynchronous A2D replay foundation.
Gateway-specific replay/delegation state is a separate future schema review.

---

## 10. Persistence ownership

Every durable byte must have one explicit owner.

Current ownership classes include:

- SecurityStore — credential/security counters/replay state authorized by its schema;
- ConfigStore — durable requested device configuration;
- HistoryStore — application observation/history backlog;
- BLE/InternalFS bond storage — framework BLE state;
- bootloader/settings — firmware lifecycle ownership.

Future durable state such as:

- gateway delegated replay slots;
- message mailbox;
- command idempotency state;
- entity cache;
- relay custody queue

requires an explicit owner, reset policy, power-cut invariant, wear budget and
partition review before implementation.

Do not share pages casually between services.

---

## 11. Power architecture

Power policy is a system coordinator, not a hidden side effect of Role.

Examples:

- animal tracker: low-power MCU, bounded RX, GNSS duty policy, low-power sensors;
- relay forwarding enabled: radio availability between local TX;
- gateway bridge/mobile search: availability prioritized over battery;
- future actuator/sensor service: explicit service-specific power requirements.

Service requests, hardware capability and power policy resolve into effective
state.

A service that is blocked/degraded by power policy must expose a reason; requested
intent is not silently rewritten.

---

## 12. Hardware/reference-platform boundary

The owned reference platform remains:

- RAK4630/RAK4631;
- nRF52840;
- SX1262;
- current WisBlock GNSS/sensor modules.

This remains the implementation and physical-validation target.

Future hardware may use another MCU, radio, GNSS or sensor.

Therefore public/persistent semantics use physical/product meaning, for example:

- frequency Hz;
- TX power dBm;
- bandwidth Hz;
- LoRa SF/coding semantics;
- tracking interval seconds;
- location source;
- sensor capability;
- battery capacity;
- measurement units.

Do not expose driver enum/register values as product semantics.

### Extraction trigger

Do **not** build a generic board HAL today.

A platform seam is extracted only when a real second platform/module is selected
and available, using the already-demonstrated responsibilities from the current
RAK implementation.

Examples of future seams that may become justified:

- radio frame TX/RX + quiesce/reconfigure;
- device identity provider;
- flash backend;
- sensor adapter;
- GNSS adapter;
- BLE/local transport adapter.

One product architecture does not require one identical hardware binary forever.

---

## 13. External reference patterns

Before freezing a new major subsystem, compare the design against mature systems
for known failure modes and operational lessons.

### LoRaWAN

Useful precedent:

- sleepy-node receive opportunities;
- frame-counter discipline;
- bounded retry/downlink behavior;
- separation of radio/link security state.

Do not import:

- mandatory network-server dependency;
- LoRaWAN join/routing semantics;
- assumptions that conflict with ORUN offline P2P/store-forward.

### Meshtastic

Useful precedent:

- separate application families/port numbers;
- position, telemetry, text, admin/control as distinct payloads;
- stable packet/message identities;
- bounded hop/dedupe ideas;
- phone/radio coexistence.

Do not import:

- generic flooding as protected-command architecture;
- shared-channel/group-key trust as ORUN command authority;
- large generic protocol machinery without measured need.

### Traccar

Useful precedent:

- devices, positions, events, geofences, commands, notifications and reports as
  separate backend concepts.

Use this lesson mainly in backend/domain modeling; do not bind firmware to a
specific server schema.

### ThingsBoard

Useful precedent:

- telemetry, alarms, profiles and RPC/command concepts separated;
- higher-priority processing can be isolated from bulk telemetry.

Do not move a heavy rule-engine model into field firmware.

### Zephyr

Useful precedent:

- driver, subsystem, application and device power ownership remain separate.

ORUN keeps its current Arduino/PlatformIO implementation; this is an architecture
lesson, not a framework migration plan.

### Field/animal tracking projects

Useful precedent:

- antenna/enclosure placement;
- GNSS behavior;
- measured range;
- measured battery life;
- real mounting/animal behavior

must be physically validated.

Open-source README or vendor claims are evidence to investigate, not proof for
ORUN hardware.

Reference links:

- LoRa Alliance specifications: https://lora-alliance.org/resource_hub/lorawan-specification-v1-0-4/
- Meshtastic protobufs: https://github.com/meshtastic/protobufs
- Traccar API/domain model: https://www.traccar.org/api-reference/
- ThingsBoard device profiles/alarms: https://thingsboard.io/docs/user-guide/device-profiles/
- Zephyr device power model: https://docs.zephyrproject.org/latest/services/pm/device.html

---

## 14. Test architecture

Validation is layered because no single PASS proves product behavior.

### 14.1 Contract/unit tests

Use for:

- codecs;
- byte layout;
- bounds;
- state machines;
- geometry;
- replay/idempotency rules;
- persistence recovery.

### 14.2 Malformed/fuzz/sanitizer tests

Use for:

- packet parsers;
- length/boundary handling;
- security envelope;
- storage recovery;
- untrusted BLE/RF input.

### 14.3 Deterministic fault injection

Use for:

- power cuts;
- torn writes;
- timeout/retry;
- late callbacks;
- reset ordering;
- queue overflow;
- stale/duplicate packets.

### 14.4 Build/static integration

Required for firmware changes:

- warnings-as-errors where applicable;
- ASan/UBSan host paths;
- PlatformIO RAK4630 build;
- memory-size comparison;
- ownership/partition guards.

### 14.5 Load simulation

Use simulated traffic/state before buying large hardware fleets.

Required representative cases:

- ~10 nodes;
- ~30-50 nodes;
- ~100-node RF-domain stress;
- normal cadence;
- alarm burst;
- relay duplication;
- backlog recovery;
- command/result retries.

This validates algorithmic and capacity behavior, not physical RF propagation.

### 14.6 Physical hardware tests

Use real devices for:

- radio timing/range;
- sleepy RX rendezvous;
- GNSS;
- sensor behavior;
- power/current;
- flash/power-cut sentinels;
- BLE/DFU;
- antenna/enclosure effects;
- relay/store-forward;
- actual field coverage.

Host PASS and build PASS are never reported as physical PASS.

### 14.7 Field validation

Real deployment evidence should measure:

- packet loss/duplicates;
- direct vs relay path;
- RSSI/SNR distributions;
- tracker battery/current;
- GNSS acquisition;
- alarm latency;
- backlog recovery;
- coverage gaps;
- user-visible stale/fresh correctness.

---

## 15. Development sequence rule

A new feature should normally follow:

```text
product behavior
-> ownership/data model
-> reference-project/failure-pattern review
-> protocol/storage/security impact
-> smallest design slice
-> independent review
-> host/fault/load tests
-> RAK build
-> physical test when behavior requires it
-> merge
```

Do not implement app/backend/firmware pieces in parallel when a shared wire,
storage or security contract is still unfrozen.

---

## 16. Product capability map

The architecture must preserve a clear path for:

| Product area | Device/service owner | Backend/app outcome |
| --- | --- | --- |
| Tracking | Tracking + Location | live/last-known location, route/history |
| GNSS | GNSS adapter/source | location quality/diagnostics |
| Activity | Sensor + Activity | moving/resting/grazing states |
| Telemetry | Telemetry service | metrics/history |
| Sensors | sensor adapters + service | typed measurements |
| Geofence/LOST | local state machine + Event | alarm/status/map |
| Alarm/Event | Event service | notification/lifecycle |
| Messaging | MESSAGE service | inbox/delivery |
| Commands | Command/Result + application owner | pending/applied/failed |
| Health | Health/Diagnostics | serviceability/device health |
| Store-forward | History/Custody owners | no-data-loss/recovery |
| Map | app/backend Entity Registry + Location | shared situational view |
| BLE/local | transport adapter | nearby/offline service |
| Gateway | bridge/custody/enrollment | local/cloud connectivity |
| Backend | registry/auth/history/routing | long-term system state |

No single firmware class or protocol packet should become the owner of this
entire table.

---

## 17. Explicit non-goals today

This architecture does not authorize:

- TLP v2 implementation merely because families are named;
- generic multi-hop mesh;
- 1000-node flat RF domain;
- speculative second-board support;
- generic HAL/plugin framework;
- SX130x/concentrator support;
- Android/backend implementation before firmware contracts are ready;
- remote actuation before authorization/replay/freshness/idempotency are closed;
- MESSAGE before secure routing/custody prerequisites;
- claiming cattle-activity accuracy before real collar/animal validation.

---

## 18. Architecture review checklist for every slice

Before changing a subsystem ask:

1. What product outcome changes?
2. Who owns the source of truth?
3. Does this confuse Role, capability, transport, identity or security authority?
4. What happens with no RF?
5. What happens with no phone/Internet?
6. What happens on reset/power cut?
7. What happens with stale/duplicate input?
8. Can untrusted input cause flash wear or side effects?
9. What is the RAM/flash/airtime/power cost?
10. What happens at ~10, ~30-50 and ~100 devices?
11. Does the change preserve current physically validated behavior?
12. Does it make a future real second hardware platform unnecessarily difficult?
13. Is a new abstraction actually required now?
14. What does the user finally see?

This checklist is architectural discipline, not permission to expand every
small task into a large redesign.
