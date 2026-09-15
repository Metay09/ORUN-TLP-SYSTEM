# ORUN Architecture Documentation Index

Status: **CURRENT documentation governance index**.
Last reviewed against branch head: `118127b7d98dcf43d562febe314a9df49474981a`.
Code-bearing pre-M6 baseline: `a9d7bde7afcce2d7a34912c6fc4cdfcbf1788986`.

This directory contains both current owner-approved rules and historical/proposed
architecture audits. They are not all equal sources of truth. This index exists
so a new engineer can tell which document governs current work without relying on
conversation history.

## 1. Source-of-truth order

For current pre-M6 architecture decisions, use this order:

1. repository `AGENTS.md` — project-wide development and compatibility rules;
2. `ORUN_CURRENT_ARCHITECTURE_RULES.md` — current owner-approved concept and
   ownership invariants;
3. the current code, tests, compatibility/golden fixtures and milestone reports
   that describe the exact commit being changed;
4. `ORUN_SYSTEM_ARCHITECTURE_V1.md`, `ORUN_ARCHITECTURE_GAP_ANALYSIS.md` and
   `ORUN_PROTOCOL_EVOLUTION_PLAN.md` — design/audit records whose useful analysis
   remains relevant, but whose older recommendations can be superseded below.

A newer owner-approved rule does not silently rewrite historical evidence. When
an older recommendation conflicts with the current rules, the current rule wins
and the conflict must be listed here until the historical document is deliberately
reissued as a new revision.

## 2. Current non-negotiable invariants

Keep these concepts separate:

`Role != Location Source != GNSS Power != Capability != Transport != Identity != Profile != User Identity`.

Also keep relay forwarding, gateway bridging, application services, hardware
presence, requested configuration, effective runtime state and system power
policy independent.

Current product direction:

- one firmware codebase;
- profiles are overridable defaults, not permanent device types;
- relay forwarding is an independent enablement axis;
- enabling relay forwarding must not disable tracking, telemetry, sensing,
  activity or actuation services on the same node;
- animal-tracker preset defaults relay forwarding OFF for battery/airtime reasons,
  but this is not a permanent architectural prohibition;
- gateway bridging and LoRa relay forwarding are independent and may coexist;
- capability support, detected physical presence, requested enablement and
  effective service state are different facts;
- normal product UI shows present/assigned capabilities and hides absent ones;
- GNSS presence does not define application/profile/network responsibility in the
  target model;
- TLP v1 bytes and its current one-hop RELAY_FORWARD behavior are frozen;
- the current one-hop v1 rule is not a permanent future-product limit;
- any future multi-hop work must be controlled and bounded, with stable message
  identity, duplicate suppression, hop/flood policy, airtime admission, security,
  mixed-fleet handling and field validation;
- no persistence/config flash allocation is authorized until the verified
  partition/ownership work is complete.

## 3. Historical/proposed document status and supersession map

### `ORUN_SYSTEM_ARCHITECTURE_V1.md`

Status: **historical/proposed architecture audit**, originally reviewed against
`aa3bbf810a37034d9a3d9066ede4bf579646adfe`.

Keep its layer separation, identity/location/persistence/security analysis,
failure model and staged-gate reasoning. The following older recommendations are
superseded for current work:

- wording that treats `NetworkRole = END_NODE | RELAY` as the permanent product
  model is superseded by an independent `relay_forwarding_enabled` behavior axis;
- wording that says animal tracker presets **must never** relay is superseded:
  the preset defaults relay OFF, but explicit validated configuration may later
  enable relay while tracking continues;
- wording that keeps one RF relay hop as the target future architecture is
  superseded: **TLP v1 remains exactly one hop**, while future protocol/network
  work may support controlled bounded multi-hop;
- profile/role names describe defaults or compatibility behavior and must not be
  used to infer physical capability, location source or power state.

Do not use these supersessions to change current v1 runtime behavior implicitly.
They define the target boundary for later reviewed configuration/network work.

### `ORUN_ARCHITECTURE_GAP_ANALYSIS.md`

Status: **historical gap register** from the same audited baseline.

The gap findings remain useful, especially G01/G02/G03/G04/G05/G07/G08/G09/G10.
Current implementation progress changes their status:

- G03/G04/G05: B2 introduced portable `GnssFix`, pure legacy POSITION mapping
  and independent `DeviceIdentity`/RAK identity boundary while preserving bytes;
- G01: B3 introduced explicit legacy behavior mapping and made relay forwarding a
  separate boolean compatibility axis; it is not yet user-configurable;
- G02: AUTO GNSS -> TRACKER / absent -> BASE remains only as frozen legacy
  bootstrap behavior; future explicit validated config must take precedence;
- G08/G09: typed config/command boundary is the next bounded implementation area;
  durable persistence still waits for G07;
- G10: source-neutral location ownership remains future work; B2 only created the
  neutral GNSS value/mapping seam.

Where the older G01 target names END_NODE/RELAY as a mutually exclusive role,
interpret it as **forwarding disabled/enabled compatibility intent**, not a new
permanent node type hierarchy.

### `ORUN_PROTOCOL_EVOLUTION_PLAN.md`

Status: **historical protocol recommendation**, not a wire specification.

Its frozen v1 facts remain authoritative: TEST/POSITION/RELAY_FORWARD layouts,
strict decoding, byte compatibility, current nested-relay rejection and current
one-hop behavior are unchanged.

The following future recommendation is superseded:

- any text that fixes the future generic envelope to an original maximum of one
  RF relay hop, or treats one-hop as the permanent target network topology.

Current direction is only that a future protocol/network milestone **may** support
bounded multi-hop. No hop count, flooding algorithm, routing algorithm or new wire
field is approved here. That milestone must first define and test stable network
message identity, dedupe/reset behavior, hop/flood limits, airtime amplification,
security/authentication and mixed-fleet compatibility. Existing TLP v1 must never
be reinterpreted to achieve multi-hop.

## 4. Current implementation checkpoint

The code-bearing commit
`a9d7bde7afcce2d7a34912c6fc4cdfcbf1788986` has owner-run evidence for:

- full host regression PASS including B1A/B2/B3 and M3/M4/M5/R2/R3/R4 suites;
- B3 portable seam built under `gnu++11` to catch the RAK GCC 7 constraint;
- `pio run -e rak4630` SUCCESS;
- RAM 13,852 bytes (5.6%), flash 139,576 bytes (17.1%) for that build;
- DFU programming SUCCESS to RAK4631;
- runtime USB `ROLE?` and `ROLE TRACKER` override path responding.

Earlier owner-operated B1B hardware evidence demonstrated real open-sky GNSS on
Tracker B and DIRECT POSITION reception on Base A. The exact B2/B3 code-bearing
commit still requires the short GNSS -> storage -> POSITION -> Base DIRECT
regression before claiming that exact commit physically preserves the whole path.
Host/build/upload success must not be reported as that hardware PASS.

## 5. Documentation maintenance rule

For every meaningful change, review whether it changes any of:

- concept ownership or dependency direction;
- compatibility/wire bytes/mixed-fleet behavior;
- storage layout, retention or reset ownership;
- RF airtime, forwarding, dedupe or topology;
- power/sleep/service availability commitments;
- capability detection or UI visibility;
- location source/freshness semantics;
- security/authentication/authorization/replay assumptions;
- physical validation status.

Update only documents actually affected by the change. Historical audit documents
should not be casually rewritten to look as if they originally reached newer
conclusions; prefer a new revision or an explicit supersession entry here.

Before merge, the change report must state code impact, architecture impact,
protocol impact, storage impact, power/RF impact, documentation updated, tests
run and remaining hardware validation.
