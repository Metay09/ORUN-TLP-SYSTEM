# M7P7C Independent Architecture Review Disposition

Date: 2026-09-21
Baseline reviewed: `main@46d7a933f63d42d84fb386035be1c03af1d0c3c4`
Branch after disposition: `docs/m7p7c-ble-application-contract`

Source: owner-provided independent Claude architecture review of PR #31. This file
records ORUN's disposition of the review against repository source-of-truth. It is
not hardware evidence, CI evidence or a claim that Claude executed code.

## Summary

No production-firmware rewrite is required by the review.

The strongest findings are accepted at the documentation/architecture level:

- real-world binding must support resource/instance scope and separate valid time
  from record/audit time;
- offline binding conflicts need explicit semantics, with stricter fail-closed
  behavior for actuator bindings;
- MESSAGE recipient/endpoint/DELIVERED semantics need to be explicit;
- presence is per installation and is only routing evidence, never delivery;
- person-location privacy must not be bypassed by using a hardware tracker;
- product/application decisions should not be owned by the BLE milestone;
- RF capacity needs modelling before a 50–100-device field claim.

Several proposed implementation details are intentionally not frozen.

## Findings

### 1. Device-to-entity binding is too narrow

Disposition: **ACCEPT WITH SCOPE CONTROL**.

The original direction used a whole-device binding. A future multi-channel
sensor/actuator node can expose several independently meaningful resources, so
the Entity Registry must be able to address a device resource/instance. The
simple tracker case remains instance `0`; firmware does not gain a speculative
generic instance framework now.

The review's bitemporal point is accepted in bounded form: preserve real-world
valid time separately from record/audit time, actor and correction/supersession
history. Observations remain tied to technical source identity and observation
time so later assignment corrections do not destructively rewrite history.

The proposed optional HistoryStore `boundary_seq` is **not frozen**. Existing
HistoryStore ticket/sequence semantics are not repurposed as an Entity Registry
clock without a dedicated review.

### 2. Offline binding conflict policy is missing

Disposition: **ACCEPT PRINCIPLE; DO NOT ADOPT A FULL CRDT/LWW DESIGN NOW**.

The application direction now permits a small append-only operation model with
stable operation IDs/base revisions, but does not select a CRDT library or vector
clock.

Conflicting overlapping bindings are explicit `CONTESTED`, not silently
trusted. For actuator/safety-relevant bindings, pending or contested assignment
blocks protected actuation until the exact technical target is confirmed.

The review's suggestion to rely on arrival-order LWW or trusted phone timestamps
is not adopted. A delayed offline write must not overwrite a newer authoritative
same-field edit merely because it arrived later, and client wall-clock time is
not security authority.

### 3. 50–100-device RF scale is beyond the current planning reference

Disposition: **ACCEPT AS A MODELLING/EVIDENCE GAP; NO RUNTIME CHANGE IN PR #31**.

Repository facts confirm the nominal planning reference is about 15 trackers,
4 relay-forwarding nodes and 2 gateway bridges, while practical development
scale is roughly 3–30 devices.

The review's ALOHA/airtime calculations are useful order-of-magnitude analysis
under their stated assumptions, but they use a 15-minute report interval. Current
`main` development default is 3 minutes; product direction also discusses longer
normal intervals. A capacity model must therefore include the current 3-minute
development default, intended 15–30-minute product intervals where applicable,
bursts, and varying relay fan-out.

The review's generic "~10% duty-cycle" example is **not** adopted as an ORUN
regulatory fact. Deployment-jurisdiction rules must be independently verified.

No hard "30–40 trackers per relay" cell limit is frozen; RF geometry and measured
collision domains determine useful infrastructure placement.

Existing relay diagnostics already count queue drops/forward outcomes. Additional
RAM-only airtime instrumentation is a reasonable separate diagnostic slice if
host modelling and existing counters prove insufficient.

### 4. K_root/backend escrow and recovery

Disposition: **RISK ACCEPTED, ASSERTION CORRECTED**.

The security ADR already says backend registration, key wrapping/escrow and the
provisioning ceremony are unfrozen. Therefore "the backend must store raw
`K_root`" is not a repository fact.

Before production credentials are written, ORUN must define the active authority
key-custody model, backup/recovery/disaster-recovery behavior and compromise
boundary. That design may use wrapped/derived material; raw backend `K_root`
storage is not preselected.

A single fleet/group authority key remains prohibited.

### 5. MESSAGE routing predicate, endpoint and DELIVERED semantics

Disposition: **ACCEPT CORE REFINEMENTS; PRESERVE OWNER ROUTING POLICY**.

Accepted:

- route choice is made by the current message custodian using fresh reachability
  state;
- presence is per authorized app installation, not merely per user;
- push token/delivery is not presence;
- recipient is a user/account (or explicit recipient scope), while an endpoint is
  an authorized app installation;
- `DELIVERED` requires one authorized recipient endpoint to authenticate,
  decrypt/validate, durably store before expiry and return an authenticated
  receipt;
- backend/gateway/BLE-buffer/`TX_DONE` custody is not delivery;
- no read receipt in MESSAGE v1.

The owner-approved baseline remains Internet-backed delivery first, LoRa fallback,
then bounded store-and-forward. Ordinary MESSAGE does not become dual-path by
default. An emergency reliability class may be designed separately later.

The review identified an additional possible no-RF route:
backend -> Internet-connected local gateway -> BLE -> recipient endpoint, even
when the phone itself has no Internet. This is recorded as a later route-policy
decision, not silently inserted ahead of LoRa without owner approval.

### 6. PERSON privacy can be bypassed by hardware binding

Disposition: **ACCEPT**.

Person-location privacy applies regardless of whether the source is phone GNSS or
a physical ORUN tracker. Binding hardware to a PERSON entity must not bypass the
personal-location authorization/consent and visibility policy.

Jurisdiction-specific legal/compliance conclusions are not frozen in this
architecture record.

### 7. Documentation scope/governance

Disposition: **ACCEPT**.

M7P7C had accumulated BLE, MESSAGE, Entity Registry and map/product decisions in
one milestone. Those later application decisions now live in
`ORUN_APP_ENTITY_MESSAGING_DIRECTION.md`; M7P7C remains focused on BLE
application/commissioning boundaries.

`ORUN_SYSTEM_ARCHITECTURE_V1.md` is historical/proposed. It is not rewritten.
The architecture index now explicitly supersedes its older optional read-receipt,
map-icon and assignment wording where current direction differs.

## Additional review points

### GATT transport seam

Disposition: **ACCEPT DIRECTION**.

Before protected application GATT writes, define a small bounded
transport-neutral request/result seam and host/USB-test it. BLE remains a thin
transport adapter. Per-connection queue limits, fragment cleanup, flood/rate
limits and malformed-input tests are implementation gates.

No exact characteristic/byte-stream layout is frozen by this review.

### Just Works pairing / bond-store denial of service

Disposition: **ACCEPT AS COMMISSIONING GATE**.

M7P7B proves stock pairing/bond persistence, not ORUN authorization. Before a
protected application GATT is exposed, test finite bond-store exhaustion and
ensure possession of a framework bond never grants application authority.

### Fresh-pairing CC310/LESC gate

Disposition: **ACCEPT**.

M7P6E fresh-pairing stress remains owner-waived/not-PASS. Commissioning is a
fresh-pairing-sensitive path, so the coexistence/serialization closure is a
production-commissioning merge gate.

### E2E private MESSAGE threat model

Disposition: **ACCEPT BOUNDARY, DO NOT FREEZE CRYPTO**.

Opaque gateways remain required. Ordinary backend storage/routing should not need
private MESSAGE plaintext. However, if the backend is the sole endpoint-key
directory, malicious key substitution is not prevented until a later verifiable
key/identity design exists.

Multi-device key distribution, phone-loss/history recovery and group messaging
remain focused later security work. Do not invent group cryptography.

The review's claim that safety/alarm events "cannot" be end-to-end confidential is
too absolute. Private MESSAGE and system/safety EVENT are separate classes; any
backend-visible escalation metadata must be designed explicitly for EVENT rather
than used to weaken private-message confidentiality.

## Result

PR #31 remains documentation-only.

No TLP v1 bytes, RF behavior, storage layout, BLE runtime/admission, production
credential path, MESSAGE runtime, Android or backend code changed as a result of
this review.

No host/build/hardware PASS is claimed by this audit.
