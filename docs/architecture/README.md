# ORUN Architecture Documentation Index

Status: **CURRENT documentation governance index**.
Last reviewed against `main@fb3a098c5bfc8b3488ba61a5ee28b57d8c5b0765` (post-M7P7A documentation checkpoint). M7P6B SecurityStore/TX nonce persistence and the M7P7A BLE flash/SoftDevice event-ownership prerequisite are implemented; on `main` BLE runtime/admission is **not** enabled. **PR #22 (`feat/m7p7b-ble-runtime-admission`, not yet merged at this checkpoint)** implements the M7P7B minimal tracker BLE runtime/admission (`docs/milestones/M7P7B.md`). Physical evidence covers real advertising/phone connection, connected past the ~10-min deadline, the current direct-event disconnect → loop-owned restart → fresh window → reconnect lifecycle, no-client close, clean cold boot, stock bond creation + power-cycle persistence/reconnect, BLE coexistence with real LoRa RX and BLE-connected RELAY RX/QUEUE/TX/TX_DONE, and a real ConfigStore→FlashMutationGate→SoftDevice async flash probe while BLE remained connected (6/6 completions, no errors/timeouts/late completions/disconnects, exact restore). The flash probe does not separately prove HistoryStore/SecurityStore client-specific mutation paths; TX_DONE is not end-to-end delivery. Owner disposition: quantitative current/power is **DEFERRED, not PASS**, because no measurement equipment is available and is accepted as non-blocking for M7P7B; GNSS coexistence remains **BLOCKED on this unit** (`GNSS: not detected`) and is owner-waived as an M7P7B merge blocker only, with later GNSS-equipped physical validation still required. Between-poll short-session timing and advertising start/stop fault injection remain host-only. Secure RF envelope, provisioning transport, ORUN application GATT, DFU and field-network/serviceability runtime remain later work.
Historical pre-M6 architecture baseline: `859ca4af0abf9f533a54227b38d2b1a5ddcfcccb`.

This directory contains current owner-approved rules plus historical/proposed
architecture audits. They are not equal sources of truth. This index tells a new
engineer which document governs current work without relying on conversation
history.

## 1. Source-of-truth order

For current architecture decisions, use this order:

1. repository `AGENTS.md` — project-wide development, compatibility and validation rules;
2. `ORUN_CURRENT_ARCHITECTURE_RULES.md` — current owner-approved concept,
   ownership and current/runtime-vs-future boundaries;
3. focused owner-approved design records for the area being changed, currently including
   `ADR_M7P6_SECURITY_ARCHITECTURE.md` and
   `ORUN_FIELD_NETWORK_DIAGNOSTICS_PLAN.md`;
4. current code, tests, golden/compatibility fixtures and milestone/audit reports
   describing the exact commit being changed;
5. `ORUN_SYSTEM_ARCHITECTURE_V1.md`, `ORUN_ARCHITECTURE_GAP_ANALYSIS.md` and
   `ORUN_PROTOCOL_EVOLUTION_PLAN.md` — historical/proposed analysis that remains
   useful unless superseded by newer rules above.

A newer rule does not rewrite historical evidence. When a historical proposal
conflicts with current rules, current rules win and the supersession should be
made explicit rather than silently editing history.

## 2. Current non-negotiable invariants

Keep these concepts separate:

`Role != Location Source != GNSS Power != Capability != Transport != Identity != Profile != User Identity`.

Also keep separate:

- relay forwarding and gateway bridging;
- hardware support/presence/health and application service enablement;
- requested configuration and effective runtime state;
- location freshness/source ownership and GNSS driver/power state;
- application state and RF delivery/contact truth;
- persistence owners and transient queues.

Current product direction:

- one firmware codebase on the current RAK4630/RAK4631 reference platform;
- profiles are overridable defaults, not permanent device types;
- relay forwarding is an independent enablement axis;
- enabling relay forwarding must not disable tracking/telemetry/sensing/activity/
  actuation on the same node;
- gateway bridging and LoRa relay forwarding are independent;
- capability presence does not imply service enablement;
- TLP v1 bytes and current one-hop RELAY_FORWARD behavior remain frozen;
- future multi-hop, if ever implemented, requires a separate bounded protocol /
  network milestone and must not reinterpret v1;
- no new durable config/activity/geofence/security storage may be allocated until
  partition/ownership is explicitly verified;
- `TX_DONE` is not delivery or network contact;
- trustworthy network-contact LOST remains security/receipt gated.

## 3. Current implementation checkpoint

### Canonical pre-M6 foundation

The B2/B3/B4 stack is merged in `main@859ca4af0abf9f533a54227b38d2b1a5ddcfcccb`.
Its frozen TLP v1 compatibility and legacy role behavior remain regression
requirements for M6.

The B4-era physical mixed-fleet direct path is recorded as PASS for the tested
image: Tracker `0E8ADE7E71531AA3` produced a direct POSITION received by unchanged
Base `09A462BD4B275BA5`. That proof does not generalize to untested relay,
power-cut, long-range, current-consumption or future M6 behavior.

### M6 software stack through M6C2

Current staged checkpoints:

```text
M6A  a4e5a9b1ae7f1143c4ec70441a51e0b1c978329d
M6B1 8353e18785cd16d62c43f0c0d5f63418c96a3d0f
M6B2 5400977c971cf118c65a134515fc75d4b8dec00e
M6C1 853622c4849658ec215ae698364c577c1133614b
M6C2 4858db8e19318ba7cf007fd94d2765b3f9084c0b
```

Only M6A is production-runtime integrated. It adds bounded RAK1904/LIS3DH
identity/config/sample/shutdown behavior and accelerometer capability projection.
The latest audit-hardened M6A image is **not yet physically validated** on the
owned RAK1904.

M6B1/M6B2 and M6C1/M6C2 are software-validated portable helpers but are not
referenced by `main.cpp`:

- activity window/features;
- activity feature eligibility;
- polygon geometry;
- multiple permitted-area union composition.

They do not prove continuous activity sampling, cattle classification, runtime
geofence state, NEAR_FENCE, hysteresis, FREE_GRAZE or LOST.

Current measured linked image after M6C2 remains:

```text
RAM   13,932 / 248,832 bytes (5.6%)
Flash 141,928 / 815,104 bytes (17.4%)
```

because the M6B/M6C host-only helpers are linker-removed from production runtime.

See:

- `docs/milestones/M6.md` for the current staged M6 validation matrix;
- `docs/audits/PRE_M6A_BRANCH_REVIEW.md` for M6A software findings/fixes;
- `docs/audits/PRE_M6_STACK_AUDIT_PACKAGE.md` for final independent audit scope.

## 4. Historical/proposed document status and supersession map

### `ORUN_SYSTEM_ARCHITECTURE_V1.md`

Status: **historical/proposed architecture audit**.

Keep its layer separation, identity/location/persistence/security analysis,
failure model and staged-gate reasoning. Older wording that treats END_NODE/RELAY
as a permanent mutually exclusive product type is superseded by independent relay
forwarding enablement. Animal tracker relay-OFF is a default, not a permanent
architecture prohibition.

Likewise, one-hop is frozen **for TLP v1**, not as a permanent future product
limit. No multi-hop design is currently approved.

### `ORUN_ARCHITECTURE_GAP_ANALYSIS.md`

Status: **historical gap register**.

The findings remain useful, but several boundaries have since progressed:

- B2 separated portable GNSS observation / legacy mapping and device identity;
- B3/B4 separated relay-forwarding runtime ownership from legacy role mapping;
- M6A added an independent accelerometer capability/driver boundary;
- durable configuration, generic second-source Location ownership, authenticated
  contact, event delivery and BLE storage prerequisites remain unresolved gates.

Interpret old role/topology wording through the current rules; do not reintroduce
role-owned forwarding or hardware-presence-owned application identity.

### `ORUN_PROTOCOL_EVOLUTION_PLAN.md`

Status: **historical protocol recommendation, not a current wire specification**.

Its frozen v1 facts remain applicable. No M6 software slice changes TEST,
POSITION or RELAY_FORWARD wire bytes. Any future packet family, ACK/contact or
multi-hop envelope requires an explicit new protocol milestone and mixed-fleet /
security review.

## 5. Documentation maintenance rule

For every meaningful change, review whether it changes any of:

- concept ownership or dependency direction;
- compatibility/wire bytes/mixed-fleet behavior;
- storage layout, retention or reset ownership;
- RF airtime, forwarding, dedupe or topology;
- power/sleep/service availability commitments;
- capability detection or UI visibility;
- location source/freshness semantics;
- activity/geofence state ownership;
- security/authentication/authorization/replay assumptions;
- physical validation status.

Update only documents actually affected by the change. Historical audit documents
should not be casually rewritten to look as if they originally reached newer
conclusions; prefer a new revision or explicit supersession entry.

Before merge, the report must state code impact, architecture impact, protocol
impact, storage impact, power/RF impact, docs updated, tests run and remaining
hardware validation.
