# ORUN Node Model — hardware, services, profiles and deployment (DRAFT)

Status: **product architecture direction, design-only; no runtime authorization**.
Owner clarification: 2026-10-09. Designed to remove the false assumption that
TRACKER, SENSOR, VALVE, RELAY and GATEWAY must be separate and exclusive devices.
No implementation, firmware migration, actuator operation or protocol changes
are introduced by this design. Independent architecture/security review required.

## 1. One physical ORUN node, composable services

**A device is an ORUN NODE** with one durable device identity. Its components
and deployment can vary. Do not invent a new firmware per combination.

Separate the following dimensions:

| Dimension | Meaning | Examples |
|---|---|---|
| Platform | MCU, radio and available power/electrical limits | RAK4631/nRF52840 with SX1262; future compatible board |
| Hardware / capability | Present, validated or explicitly assigned modules | GNSS, accelerometer, temperature, soil moisture, pressure, GPIO, suitable valve driver, BLE/USB |
| Requested service | Independently enabled function | Own-location tracking, sensor sampling, actuator/valve control, LoRa relay, gateway bridge, Gateway durable custody |
| Effective service | What actually runs after hardware, safety, energy, policy and security checks | ENABLED / DEGRADED / BLOCKED / DISABLED with reason |
| Profile | Editable initial collection of defaults, never a hardware identity or protocol role | ANIMAL_COLLAR, FIXED_SENSOR, IRRIGATION_STATION, FIXED_INFRA, MOBILE_BRIDGE |
| Deployment | Physical location/mobility, energy source and upstream connectivity | Mounted on animal, solar fixed, vehicle powered, phone-tethered, USB-host-tethered |

A capability being physically present **does not automatically enable** it.
A role/profile label **does not override** actual capabilities or user intent.
The existing legacy `NodeRole { TRACKER, RELAY, BASE }` remains frozen for
compatibility until an explicit, tested migration changes the requested-config
source. Do not derive actuator rights, gateway custody or trust from role labels.

Service dependencies are explicit:
- **Own observations**: sensors and/or Location source plus cadence/policy.
  A sensor-only node needs no GNSS.
- **Actuator/valve**: assigned electrically suitable output/driver, sensed or
  verifiable state where available, safety interlocks, authorized local/remote
  control and operation policy. No GNSS dependency.
- **Relay**: RF receive/forward capability plus explicit listen/airtime and
  power consent. No GNSS, sensor, valve or local durable-custody prerequisite.
- **Gateway bridge**: RF transport + a connected phone/host/Edge via an
  approved local connection. A fixed or moving Gateway follows the **same**
  bridge/custody protocol; mobility only alters deployment/energy/availability.
  An RAK4631 does not acquire native Wi-Fi or Internet by being a Gateway.
- **Gateway durable custody**: separately authorized authenticated protected
  data acceptance and physical owner/recovery proof. Relay != custody; bridge
  != durable acceptance; Edge acceptance/ACK proofs are separate stages.

## 2. Sample configurations — NOT mutually exclusive product SKUs

The rows show *requested* services, not proven production combinations.

| Example deployment | GNSS/own location | Other own sensors | Valve/actuator | LoRa Relay | Gateway bridge |
|---|---:|---:|---:|---:|---:|
| Animal collar with GNSS and movement | optional/on if fitted | optional/on | off | **off by default** | off |
| Fixed sensor station | off unless fitted | on | optional | optional | optional |
| Irrigation / valve controller | off unless fitted | on or optional | on | **optional/on** | optional |
| Fixed relay with environment sensors | optional | **on** | optional | **on** | optional |
| Vehicle/mobile bridge | optional | optional | optional | optional/on | **on, when attached host available** |
| Fixed gateway/bridge | optional | optional | optional | optional/on | **on, when attached host available** |

Examples:
- `animal-17`: Location(GNSS)=ON, accelerometer=ON, relay=OFF.
- `irrigation-3`: soil-moisture=ON, valve=ON, relay=ON,
  gateway=OFF. This is an intended supported combination, **not yet tested**.
- `relay-weather-2`: temperature=ON, relay=ON, GNSS=OFF.
- `vehicle-bridge-1`: gateway bridge=ON (USB/phone host), relay=ON,
  own location=OPTIONAL. Mobility is a deployment property, not a second
  gateway protocol or new firmware.

The configuration model must allow setting each service independently.
Profiles are defaults and can be overridden only within validated hardware,
regional RF, battery/power, safety and authorization constraints.

## 3. Data and 128 KiB candidate flash model

The 128 KiB candidate is **one physical region** under a proposed single
SharedDurablePool, **not** static 64/64 allocation and not a confirmed available
128 KiB payload. Some physical sample pages were nonblank and D1/D2 remain OPEN.

Two logical **storage ownership classes**:
1. **OWN_OBSERVATION**: this node's GNSS, activity, environmental sensor
   observations and eligible *reported* equipment measurements/events.
   Capacity policy for ordinary finite-history observations can use bounded
   oldest-eligible-first eviction with explicit retention-loss reporting.
2. **FOREIGN_CUSTODY**: exact authenticated objects accepted by this node
   as Gateway durable responsibility. These are **pinned** until the specified
   authenticated durable downstream release; capacity pressure must reject
   new responsibility rather than delete pinned records.

**Actuator distinction:** command authorization/replay state, pending effects,
verified/uncertain actuator outcomes, and safety-critical audit obligations
are **not automatically treated as disposable own sensor history**.
Define their semantics, retention, and physical owner separately before
placing any such records into the pool; do not evict an unresolved safety
obligation as ordinary telemetry. No valve control runtime is implemented.

**Relay-only** forwarding uses bounded volatile RF queue/deduplication and
does not consume durable-custody space or grant an ACK.

All service combinations share one physical flash mutation authority, must
not independently format/erase the same pages, and may not destroy stored
objects when toggled or when profiles change. Preserve existing standalone
SecurityStore, ConfigStore, BLE bond, Geofence and legacy History owners.
Any on-flash format and re-baseline needs specific validation/consent.

## 4. Single LoRa RF resource — concurrency and field reality

A single SX1262 is **half-duplex**. A node may both originate observations
and relay packets, but never transmit and receive simultaneously on that
radio. Radio arbitration coordinates queueing, own TX, relay forwards,
gateway receive and retransmission opportunity.

- Relay enabled: continuous RX **between** local TX in the current ORUN
  service contract; cost in energy, airtime and listen-availability is real.
- Default **relay OFF on animal collars**. Fixed supply, solar, vehicle or
  adequately evaluated power systems are the preferred relay deployments.
  Do not silently suppress user-enabled relay under a hidden power heuristic.
- Give latency-sensitive alarm/control traffic explicit bounded priority
  without allowing long relay backlogs to starve local control/safety or
  treating RF delivery as guaranteed. Queues, backpressure, dedupe, duty
  limits, anti-loop protection and fairness must be proven per service.
- Current frozen TLP v1 Relay forwards direct POSITION packets only, one hop,
  RAM queue; it does **not** generically relay future actuator COMMAND,
  protected observation or sensor records. Extending v2 relay support needs
  reviewed packet/security policy, not blanket forwarding of unknown bytes.

## 5. Valve and actuator control — separate safety/security gate

A valve is a **controlled actuator service**, not an automatic privilege of
a SENSOR, RELAY or GATEWAY device. An ORUN node may **request** valve+relay
simultaneously, but no production authorization is implied.

Required prior to enabling:
1. Explicit hardware/driver identity, output voltage/current, isolation,
   valve type (normally open/closed, latching/nonlatching), feedback inputs,
   power and physical emergency/manual controls.
2. Define fail-safe **per installation** for reset, brownout, radio loss,
   disconnected host, stalled firmware, sensor faults and watchdog; do not
   universally assume OFF is safe for every installation. Safety output must
   not depend on successful RF relay or Cloud availability.
3. Local interlocks and priority: authorized physical/local safety rules
   always outrank relay queue, sampling and external instructions. Explicit
   command timeout, max energization where appropriate and fault reporting.
4. For remote commands: authenticated identity and scoped authorization,
   freshness/replay prevention, unique command ID, target binding,
   idempotence, requested vs applied vs observed state distinction, bounded
   retries and failure reporting. `TX_DONE`/network ACK does not equal valve
   movement. Secure command/response protection may require a separately
   reviewed TLP v2 wire contract; no unauthorized control through v1 relay.
5. Measure RF load, switch-induced supply noise, watchdog behavior,
   brownouts, security attacks and interaction with relay RX/TX on **actual**
   hardware before field deployment. No actuator test/energization is
   authorized by this architecture document.

## 6. Status and migration checkpoints

**Known working / validated:** GNSS Tracker position acquisition, local
HistoryStore append and direct position TX on RAK-1; a second RAK
has physically received POSITION in prior tests. The v1 Relay forwarding
path and independent requested-config fields have code/host coverage.

**Not proven / not enabled as a product:** combined simultaneous own
sensor/Tracker TX + Relay RX/forward on physical device; arbitrary
sensor payloads; valve actuator output; multi-feature user-configurable
persisted profile editor; full mobile/fixed Gateway custody; shared 128 KiB
physical format/admission and secure v2 forwarding. Existing production
requested settings are still projected from the legacy NodeRole behavior.
Two development radios do not by themselves prove a 3-hop RF end-to-end
path; add a third radio or documented instrumentation before signoff.

**Next validated milestones:**
- Independently review this capability/service/profile model, radio
  arbitration and valve safety/authorization boundaries.
- Extend requested/effective config with independent, persisted service
  intentions and reasons *without changing legacy behavior until migration*.
- Host-test combined local producer + Relay, then hardware evidence and
  power budget. Future sensor/actuator services remain opt-in and gated.
- Independently implement and audit SF5D4 SharedDurablePool, D1 physical
  allocation and D2 combined ownership; never conflate Relay with custody.

## 7. Naming rule for user-facing surfaces

**Device:** `ORUN node`, one stable device ID.
**Available hardware:** `GPS, accelerometer, moisture sensor, valve driver ...`
**Services:** `Location, Sensor Reports, Valve Control, Relay, Gateway Bridge,
Gateway Durable Custody`.
**Profile:** `Animal, Fixed Sensor, Irrigation, Relay Infrastructure,
Fixed Gateway, Mobile Gateway` (editable bundle only).

Avoid user-facing exclusive "this device IS a Tracker/Relay/Base" states.
Show toggles only when supported; when a requested service is unavailable,
show a clear blocked/degraded reason. Keep existing legacy firmware role
values unchanged until a controlled migration.
