# ORUN TLP - Project Instructions

## Project

ORUN TLP is an open-source livestock tracking, geofencing, activity monitoring
and field-search system based on RAKWireless WisBlock hardware and private
LoRa P2P communication.

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

Main profiles:

- TRACKER
- RELAY
- BASE
- MOBILE / SEARCH

Hardware capabilities should be automatically detected at boot when practical.

Examples:

- RAK12500 GNSS present / absent
- RAK1904 accelerometer present / absent

Profiles must not artificially disable detected hardware.

---

## Radio Architecture

Use private LoRa P2P.

DO NOT introduce LoRaWAN unless explicitly requested later.

Topology:

TRACKER -> BASE

TRACKER -> RELAY -> BASE

Maximum relay hop count:

1

Trackers NEVER relay packets belonging to other trackers.

BASE, RELAY and MOBILE prioritize radio availability over energy saving.

Animal TRACKER nodes prioritize battery life while maintaining reliable
communication.

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

RELAY:

- LoRa RX continuous
- prioritize availability

MOBILE SEARCH:

- LoRa RX continuous
- maximum useful search performance
- battery saving not a priority

TRACKER:

- low-power MCU operation
- configurable GNSS policy
- short RX windows where appropriate
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
8. Report exactly what changed.
9. Report exactly what was tested.
10. Clearly identify anything that still requires physical hardware testing.

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

Architecture changes must
