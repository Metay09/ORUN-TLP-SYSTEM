# ORUN TLP - Project Instructions

## Project

ORUN TLP is an open-source low-power field network for tracking, sensing,
events and, in later milestones, control and short human messaging over
private LoRa P2P. Livestock tracking, geofencing, activity monitoring and
field search are the first serious application and remain the current priority.

RAKWireless WisBlock is the owned, first-class REFERENCE PLATFORM. Hardware
portability is a design constraint, not a request to implement other boards.
Keep one firmware codebase, small explicit boundaries and the existing
Arduino/PlatformIO environment. Do not add speculative HALs or drivers for
hardware we do not own.

Initial real system:

- 1 TRACKER on an animal
- 1 RELAY
- 1 BASE

The RELAY may temporarily be converted to MOBILE SEARCH mode.

Primary priorities:

1. Reliable operation in hilly rural terrain.
2. Low power consumption on animal TRACKER nodes.
3. Reliable LoRa communication.
4. Maintainable and testable firmware.
5. Recoverability after failures.
6. Configurability without reflashing firmware.

---

## Hardware

### TRACKER

- RAK4630 / RAK4631
- Nordic nRF52840
- Semtech SX1262 LoRa
- RAK12500 GNSS / u-blox ZOE-M8Q
- RAK1904 / LIS3DH accelerometer
- RAK19007 WisBlock Base
- 1S Li-ion/LiPo battery
- WisBlock Unify solar enclosure

### RELAY

- RAK4630 / RAK4631
- additional GNSS/accelerometer hardware optional

### BASE

- RAK4630 / RAK4631
- later connected to an Android phone over BLE

---

## Universal Firmware

There must be ONE firmware codebase.

Do NOT create separate tracker, relay and base firmware projects.

Node behavior must be selected through configuration, profiles and services.

Current legacy role names are TRACKER, RELAY and BASE; MOBILE / SEARCH is
planned application behavior. Preserve current runtime behavior until an
explicit migration task authorizes changes.

Keep device identity, hardware platform, network forwarding responsibility,
application/profile, capabilities, enabled services, location source/ownership,
GNSS power, system power policy and transport separate. User identity is not
device identity. Profiles are overridable default bundles, not protocol or
hardware restrictions. A gateway bridge and LoRa relay forwarding are independent
responsibilities that may coexist on one device.

Relay forwarding is an independent enablement axis. Enabling relay forwarding
must not disable tracking, telemetry, sensing or actuation services on the same
node. The animal-tracker preset defaults relay forwarding OFF for battery and
airtime reasons, but this is a preset default, not a permanent architecture
restriction. Preserve the current legacy TRACKER/RELAY/BASE behavior until an
explicit configuration migration authorizes runtime changes.

Do not extend the legacy role enum for every new application. Future explicit
configuration must take precedence over hardware-based AUTO suggestions.
Role-named power and planned BLE policies below describe application defaults
and availability commitments, not hardware-capability restrictions. BLE remains
an M7 implementation task; do not enable it before its storage prerequisites.

Hardware capabilities should be automatically detected at boot when practical.

Examples:

- RAK12500 GNSS present / absent
- RAK1904 accelerometer present / absent

Firmware support, physical presence and service enablement are different facts.
For optional digital hardware, bounded probing should verify the device identity
or protocol response when practical rather than infer a sensor from an address
alone. Hardware that cannot identify itself, such as a generic analog input or
dry contact, may require an explicit configured assignment.

Normal product UI should show capabilities that are currently present/assigned
and hide capabilities that are absent, so a universal firmware image does not
create irrelevant controls. Diagnostics may retain bounded detection/failure
information without making an absent sensor appear present.

Profiles must not hide or falsify detected capabilities. Enabled services and
power policies may explicitly leave detected hardware inactive; for example,
a GNSS-equipped relay-capable node may use PHONE location with GNSS OFF. Role
alone must not determine hardware presence or active location source.

Location is separate from GNSS. Only the configured active source (or an explicit
AUTO ownership policy) may update the active location. Invalid/empty phone data,
disconnect, permission loss or source OFF must not erase the last valid point.
Use explicit validity, source, time/freshness and authorized CLEAR semantics;
never use 0,0 as a missing-location sentinel. Persisted last-known values must
not be presented as live after reboot or reconnection.

---

## Radio Architecture

Use private LoRa P2P.

DO NOT introduce LoRaWAN unless explicitly requested later.

Current validated TLP v1 topology includes:

TRACKER -> BASE

TRACKER -> RELAY -> BASE

Current TLP v1 relay forwarding is exactly one RF relay hop and rejects nested
RELAY_FORWARD envelopes. Preserve this byte/runtime compatibility.

The one-hop v1 rule is NOT a permanent product-architecture limit. A future
reviewed network/protocol milestone may introduce controlled, bounded multi-hop
forwarding with stable message identity, duplicate suppression, explicit hop/flood
policy, airtime admission, security and mixed-fleet rules. Do not implement
unbounded flooding or silently reinterpret TLP v1.

Current legacy TRACKER behavior does not relay other nodes' packets. Future
explicit configuration may allow a tracking/telemetry/sensing/actuation node to
also relay while continuing to originate its own application data.

When relay forwarding is enabled, radio availability is a service commitment:
RX remains available continuously between local transmissions. Do not silently
disable user-enabled relay forwarding because of a hidden energy heuristic; an
incompatible future power policy must be explicit and observable.

BASE, relay-enabled and MOBILE nodes prioritize radio availability over energy
saving. Animal tracker presets prioritize battery life and therefore default
relay forwarding OFF while maintaining reliable communication.

Initial RF candidate:

- Frequency: 869.525 MHz
- Bandwidth: 125 kHz
- Spreading Factor: SF11
- Coding Rate: 4/5

RF parameters MUST remain configurable.

Do not silently hard-code regulatory assumptions.

---

## TLP Protocol

TLP means ORUN Tracking Link Protocol.

LoRa packets must use a compact binary protocol.

Do NOT use JSON over LoRa.

Every packet must be versioned.

Plan for fields such as:

- protocol version
- packet type
- source/device ID
- sequence number
- flags
- timestamp when relevant
- payload
- authentication/integrity information

Normal telemetry does NOT require ACK for every packet.

Critical messages use ACK and controlled retry.

Critical examples:

- geofence violation
- LOST state
- critical battery
- configuration commands
- important command responses
- search-related critical operations

Duplicate packets must be safely detectable.

---

## Tracker Storage

Important TRACKER records must be stored locally before transmission.

Target approximately 1-2 weeks of compact local history where flash capacity
permits.

Use a circular/ring log.

Avoid excessive flash erase cycles.

Priority order:

1. Critical events
2. Current live data
3. Historical GNSS records
4. Historical activity summaries

Live data must not wait behind a large historical backlog.

When connectivity returns, old records should be transferred gradually.

Records confirmed delivered must not be continuously retransmitted.

---

## GNSS

Tracking/report interval must be configurable.

Normal position flow:

prepare/wake GNSS
-> obtain fresh valid fix
-> validate quality
-> store locally
-> perform local geofence check
-> transmit
-> enter appropriate low-power state

Never present an old position as a fresh position.

Preserve GNSS RTC/backup state when practical to improve warm/hot starts.

For long intervals, use GNSS sleep/backup behavior.

For very short intervals, do NOT blindly power-cycle GNSS.
Continuous tracking or suitable GNSS power-save operation may be more
appropriate.

If fresh fix acquisition takes longer than the requested interval:

- do not start overlapping GNSS acquisitions
- do not send stale coordinates as fresh
- continue the active acquisition until timeout
- report fix failure/age when appropriate

Track useful GNSS diagnostics:

- TTFF
- satellite count
- HDOP or equivalent quality
- valid/invalid fix
- fix age
- acquisition failure count

Conceptual defaults are configurable, not hard-coded:

- normal moving: approximately 15-30 minutes
- stationary report: approximately 60 minutes
- stationary maximum fresh-position age: 6 hours
- near geofence: approximately 5 minutes
- outside geofence: approximately 2-5 minutes
- LOST trigger: OUTSIDE + approximately 3 hours without network contact

---

## Accelerometer and Activity

RAK1904 / LIS3DH is expected on TRACKER hardware.

Initial target:

- approximately 10 Hz
- low-power operation
- FIFO/interrupt usage where beneficial

Initial cattle behavior targets:

- stationary/resting
- grazing
- walking
- later additional behavior classes

Do NOT continuously transmit raw XYZ data over LoRa.

Behavior processing must be lightweight and power-aware.

Research-derived cattle behavior models may be used initially.

Do NOT claim validated behavior accuracy until verified with the actual collar
mounting and animals.

---

## Geofence

Geofence evaluation must work locally on the TRACKER.

Support polygon geofences.

A tracker may have multiple simultaneously permitted polygon areas.

States:

- INSIDE
- NEAR_FENCE
- OUTSIDE

Use GNSS quality filtering, hysteresis and repeated valid fixes where useful to
avoid false alarms.

FREE_GRAZE:

- manually enabled
- optionally timed
- normal geofence violation alarm disabled
- normal tracking continues
- configurable critical-distance alarm may remain enabled

---

## LOST and SEARCH

TRACKER must be able to enter LOST mode autonomously.

Initial conceptual trigger:

OUTSIDE + configurable period without successful network contact

Initial default:

approximately 3 hours

All LOST parameters must remain configurable.

MOBILE SEARCH:

- RELAY hardware may temporarily become MOBILE SEARCH
- LoRa RX remains continuously active
- BLE remains available to Android
- phone GNSS may provide mobile-node position
- internet must NOT be required
- mobile-node power saving is NOT a priority
- finding the animal is the priority

Animal TRACKER still uses sensible battery protection.

---

## BLE

TRACKER:

- BLE starts after boot
- boot BLE window is 15 minutes
- if a BLE connection exists when timeout expires, keep BLE active
- if no connection exists, disable BLE
- BLE may later be enabled remotely over LoRa
- BLE should normally remain off during field operation

BASE and MOBILE may keep BLE continuously active.

USB, BLE and LoRa configuration should ultimately use the same
ConfigManager/CommandManager logic.

Do not create three separate configuration systems.

---

## Power Policy

Power saving primarily applies to animal TRACKER nodes.

BASE:

- LoRa RX continuous
- prioritize availability

RELAY forwarding enabled:

- LoRa RX continuous between local transmissions
- prioritize forwarding availability
- user-enabled forwarding must not be silently disabled by hidden power policy

MOBILE SEARCH:

- LoRa RX continuous
- maximum useful search performance
- battery saving not a priority

ANIMAL_TRACKER preset:

- relay forwarding OFF by default
- low-power MCU operation
- configurable GNSS policy
- short RX windows where appropriate when relay forwarding is OFF
- accelerometer low-power operation
- staged low-battery protection

---

## Android / Backend / Web

Do NOT build these before firmware foundations are stable.

APK is NOT required for M0 or M1.

Later Android goals:

- BLE provisioning
- BLE configuration
- BASE bridge
- offline-first local database
- server synchronization
- mobile search
- operation without internet

Remote server:

- Debian
- Docker available
- Tailscale available

Permanent historical storage belongs on the server.

---

## Milestones

### M0

- repository structure
- PlatformIO configuration
- RAK4630 firmware builds successfully
- basic USB Serial boot output

### M1

- two RAK4630 nodes exchange LoRa P2P TEST packets
- receiver reports sequence number, RSSI and SNR over USB Serial

### M2

- fresh GNSS fix -> compact LoRa packet

### M3

- low-power sleep/wake
- configurable tracking interval
- robust GNSS state machine

### M4

- flash store-and-forward
- circular history
- backlog handling

### M5

- direct BASE reception
- RELAY forwarding
- duplicate handling
- relay timing

### M6

- accelerometer
- animal activity processing
- geofence
- operational states

### M7

- BLE provisioning/configuration
- BLE firmware update / DFU

### M8

- Android BASE
- backend
- web UI

Do NOT jump ahead unless a dependency requires it.

---

## Development Discipline

For every task:

1. Inspect the repository before editing.
2. Read relevant documentation first.
3. Make the smallest coherent change that completes the requested milestone.
4. Do not rewrite unrelated working code.
5. Run relevant builds and tests.
6. Fix build/test failures before declaring success.
7. Review git diff and git status.
8. Review architecture/protocol/ownership/milestone documentation for staleness;
   update only the documents actually affected by the change.
9. Report exactly what changed.
10. Report exactly what was tested.
11. Clearly identify anything that still requires physical hardware testing.

For firmware work always run the appropriate PlatformIO build.

Compilation success does NOT prove physical hardware behavior.

---

## Cost / Model Discipline

Model selection is controlled externally by the task prompt.

Do not assume a more expensive model is needed.

Avoid unnecessary long investigations during routine tasks.

Do not spawn unnecessary sub-agents.

Do not perform unrelated refactors.

Do not expand the requested milestone with future features.

Prefer:

inspect
-> implement
-> build
-> test
-> fix
-> concise report

At the end of a milestone, create a short milestone report so later Codex
sessions do not require a huge conversation history.

---

## Git Discipline

Use small, reviewable changes.

Suggested commit prefixes:

- feat:
- fix:
- refactor:
- test:
- docs:
- build:
- chore:

Never commit:

- passwords
- API keys
- LoRa network secrets
- private device keys
- Tailscale credentials
- signing secrets
- real .env secret values

Use example/template files for secret configuration.

Never force-push main.

Never rewrite Git history unless explicitly requested.

During early milestones:

- do NOT push to GitHub unless explicitly requested
- do NOT create commits unless explicitly requested

Before committing:

- inspect git diff
- run relevant tests/build

---

## Dependencies

Prefer mature and maintained dependencies.

Do not add large frameworks for small problems.

Firmware dependencies must be evaluated for:

- flash usage
- RAM usage
- maintenance state
- power implications

Document important dependencies and why they are required.

---

## Documentation

Protocol changes must update protocol documentation.

Architecture changes must update the relevant architecture documents and
record compatibility, wire, persistence and hardware-validation impacts.

`docs/architecture/ORUN_CURRENT_ARCHITECTURE_RULES.md` records the current
pre-M6 concept/ownership invariants and explicitly distinguishes current legacy
runtime from target configuration semantics. Review it for every architecture,
configuration, capability, forwarding, location, power or service change.

See `docs/architecture/ORUN_SYSTEM_ARCHITECTURE_V1.md`,
`docs/architecture/ORUN_ARCHITECTURE_GAP_ANALYSIS.md` and
`docs/architecture/ORUN_PROTOCOL_EVOLUTION_PLAN.md` for the broader proposed
target boundaries and staged migration. Where older proposed text conflicts with
a newer owner-approved rule in `AGENTS.md` or
`ORUN_CURRENT_ARCHITECTURE_RULES.md`, update the older document in the same
bounded change before merge rather than leaving a silent contradiction.

Preserve M0-M5 and R1-R4 guarantees, startup identity safety and portable
64-bit device-ID logging. Do not rewrite working subsystems for architectural
purity. Do not change legacy wire bytes without an explicit compatibility plan.
TX_DONE is not delivery, command execution or confirmed physical state.

Configuration, durable state, history, security material and transient queues
need explicit owners and reset/retention policies. The current history region
0xED000..0xF4000 is exclusively owned; never assume nearby flash is free.
Before BLE/DFU, resolve SoftDevice flash completion and InternalFS/bond storage
ownership without removing existing safety guards.

Security, command/actuator safety, messaging, additional hardware and backend
work remain gated future milestones. Do not invent cryptography, infer confirmed
actuator state from a transmitted command, or let chat/backlog starve critical
and live traffic. Local field operation must not unnecessarily depend on a server.
