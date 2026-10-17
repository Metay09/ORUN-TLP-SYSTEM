# ORUN TLP

ORUN is an open-source, low-power private LoRa P2P field-network project. Livestock location, activity, geofence awareness and field search are its first priority; telemetry, messaging and safe remote control are staged product directions, not necessarily working features.

The owned reference platform is RAKWireless WisBlock (RAK4630/RAK4631). The project favors **one firmware codebase** with independently requested services and detected hardware capabilities, rather than separate permanent Tracker/Relay/Base product types. There is **one Gateway service**; fixed or mobile describes the installation, not another device class.

## Start here

| Question | Read |
| --- | --- |
| What is the product intended to do? | [Product brief](PRD.md) |
| What rules must coding agents and contributors follow? | [Project instructions](AGENTS.md) |
| What is the approved overall architecture? | [Product/system architecture](docs/architecture/ORUN_PRODUCT_SYSTEM_ARCHITECTURE.md) |
| Which design and ownership rules are current? | [Current architecture rules](docs/architecture/ORUN_CURRENT_ARCHITECTURE_RULES.md) |
| What is implemented, tested, planned or deferred? | [Feature/evidence inventory](docs/architecture/ORUN_FEATURE_AND_STATUS_INVENTORY.md) |
| Where are the focused architecture decisions? | [Architecture index and source-of-truth order](docs/architecture/README.md) |
| Where are implementation and review records? | [Milestones](docs/milestones/) and [audits](docs/audits/) |
| Where are the existing wire contracts? | [Protocol](protocol/) |

## Important distinctions

- **Planned is not implemented.** An approved design, passing host test, successful firmware build and physical validation are different evidence levels.
- **Relay is not Gateway.** RF forwarding, connected Gateway bridging, Gateway/Edge custody and backend delivery have distinct responsibilities and proofs.
- **TX completion is not delivery.** Historical/replayed data must not be represented as newly observed live data.
- **Current code and compatibility matter.** TLP v1/legacy behavior is protected until an explicit reviewed migration.
- **No new hardware abstraction for hypothetical boards.** Preserve current RAK reference-platform behavior and the approved portable boundaries.

The [product brief](PRD.md) is a short navigation document, **not** a new source of implementation authority. The existing `AGENTS.md`, owner-approved architecture, focused contracts, code/tests and milestone evidence retain their established roles. Do not infer today's production support from a product aspiration.

## Development workflow

Start with `AGENTS.md`, the architecture index and the relevant milestone. Make narrowly scoped changes; document their protocol, persistence, RF/power, security and hardware-validation effects. Keep failed/unperformed checks visible. Propose changes via Pull Request; no documentation-only approval silently authorizes firmware, RF, security or storage changes.

A shared UI design system can be introduced when the real mobile/web product surface is being designed; it should not be invented as a present firmware requirement.
