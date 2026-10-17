# ORUN — Product Requirements Brief

**Status:** Documentation-only product orientation, not a new architecture approval, implementation claim or release specification.
**Prepared:** 2026-10-17.
**Authority:** The existing [project instructions](AGENTS.md) and [architecture source-of-truth order](docs/architecture/README.md) take precedence. For product decomposition, use [ORUN Product/System Architecture](docs/architecture/ORUN_PRODUCT_SYSTEM_ARCHITECTURE.md). For exact capability status and evidence, consult the [feature/evidence inventory](docs/architecture/ORUN_FEATURE_AND_STATUS_INVENTORY.md) **and verify the current code/PR/milestone head**; inventories are dated snapshots.

## 1. Purpose and users

ORUN aims to provide reliable answers about animals and field devices across low-power, intermittently connected, hilly rural areas using private LoRa P2P. Initial users are livestock owners/field operators and people deploying and maintaining nodes, relays and connected gateways. Future service families may support other field telemetry and controlled actuation, but livestock tracking remains the first serious use case.

The product outcome is an accurate, understandable account of **what was observed, when, through which route, and with what delivery confidence**—not just a radio packet or a green test.

## 2. Target product outcomes (not a list of shipped features)

| Area | Required user-visible outcome |
| --- | --- |
| Location and freshness | Show an accepted position with observation time, source/quality and honest live, stale, historical or last-known semantics. Do not relabel a replay as a new fix. |
| Livestock activity | Provide useful bounded activity summaries alongside selected report intervals; any grazing/movement classification requires real field validation before accuracy claims. |
| Geofence and alarm | Express allowed-area state and meaningful change/critical events. Trustworthy LOST/contact and remote alarms require the approved security and receipt evidence. |
| Low-power field network | Allow configured tracking and optional Relay forwarding under explicit RF, power, duty-cycle, queue and retry policies; distinguish direct from relayed paths. |
| Gateway and offline transfer | A single Gateway function may be fixed or mobile and requires an actual phone/host/Edge connection for bridging. Keep Gateway custody, Edge durability and backend acceptance distinguishable. |
| Store and forward | Retain eligible observations through outages with bounded storage and replay, deduplication and authenticated custody/receipt gates. Never release records based solely on local TX completion. |
| Configuration and support | Expose independent requested/effective services, device capabilities and observable blocked reasons. Evolve USB/BLE and future LoRa operations toward shared application owners with secure protected mutation. |
| App/backend experience | Eventually provide clear map/animal/device status, alarms, history and service setup; do not represent a future mobile app/backend as deployed today. |

## 3. Product model

- **One ORUN device/node identity** with detected hardware capabilities. Do not expose old `TRACKER / RELAY / BASE` compatibility roles as permanent product types.
- **Separately enabled services** such as location tracking, sensor reporting, activity, Relay and Gateway, subject to implementation and capability constraints. User profiles are editable defaults, not independent firmware variants.
- **Gateway is one function**, with fixed/mobile installation metadata; a receiving legacy BASE is not automatically a functioning connected Gateway.
- Keep device identity, service intent, effective state, location source, GNSS/system power, network transport, security authority and data custody separate.

## 4. Non-functional requirements and boundaries

- **Reliability:** tolerate RF gaps, duplication, reboot, partial handoff and power interruptions without manufacturing delivery or freshness claims.
- **Power and RF:** protect animal-node battery life and shared-channel airtime, including relay/ACK, downlink windows, collision and backlog effects. Do not assume an unmeasured battery lifetime or guaranteed 100-node capacity.
- **Safety and security:** protected configuration, commands, receipts, anti-replay and actuation require their focused contracts and validation before production enablement.
- **Maintainability:** preserve the one-firmware-codebase model, bounded queues/memory, explicit state/storage owners and existing RAK/Arduino/PlatformIO boundaries. Do not add speculative board support.
- **Compatibility:** retain frozen TLP v1 bytes, legacy migration rules and persisted-data safety until an authorized, tested cutover.
- **Scale validation:** use approximately 10 nodes for baseline, 30–50 for meaningful load and ~100 per RF domain as an engineering stress goal, **not** a guaranteed deployment promise. 1000 devices are not a flat single-domain target.

## 5. What counts as done

Each feature or milestone must state separately:

1. Intended product behavior and affected service/owner.
2. Architecture/protocol/storage/security/power/RF impact and backward compatibility.
3. Exact code and test evidence, plus hardware/field evidence **or an explicit not-tested status**.
4. Observable failure, stale/duplicate/retry and outage behavior.
5. Whether the capability is **designed**, **host-validated**, **production-integrated**, **physically qualified**, or **still deferred**—never inferred from an earlier PASS or another branch.

Use the applicable [milestone](docs/milestones/) and [audit](docs/audits/) records for acceptance. This brief does **not** authorize an ACK format, timing policy, flash layout, protected command, new UI, or other implementation detail.

## 6. Not promised by this brief

- Finished Android/web/backend/Gateway product paths merely because architectural direction exists.
- Multi-hop mesh, SX130x/concentrator support or a guaranteed large flat RF fleet.
- Exact battery endurance, sensor-classification accuracy or field reliability before quantitative/physical testing.
- Executable remote actuation or secure delivery without validated authentication/authorization and durable-evidence boundaries.

## 7. Change control

When product goals change, update this short summary **after** the relevant owner-approved architecture or milestone decision. Keep the detailed contract in its current canonical document; link to it instead of duplicating wire bytes, timings, numeric flash budgets, state-machine implementations or test reports here. Historical records stay historical, and no existing documentation is deleted or silently superseded by this brief.
