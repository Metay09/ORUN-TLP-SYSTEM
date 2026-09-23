# ORUN Architecture Documentation Index

Status: **CURRENT documentation governance index**.
Last reviewed against `main@699a74ea66cf3d22d9f1644fbf0f786f0092bd56` plus the M7P6E follow-up evidence on PR #33 (code-bearing hardware-tested head `fb1b9408ae3c49cd3f6c26002a6d583aa18bb592`). M7P7E requester ownership is merged and M7P7B/C remain the governing BLE runtime/application-boundary prerequisites. M7P6B SecurityStore/TX nonce persistence, M7P7A BLE flash/SoftDevice event ownership, M7P7B minimal tracker BLE runtime/admission, and the M7P6C/D/E security proof/pre-wire/coexistence foundation remain current. The corrected M7P6E test-only fresh-pairing LESC/CC310 gate is **physically PASS for the scoped pinned RAK4631/framework/probe path**; this does not prove arbitrary CryptoCell thread-safety or activate production secure-envelope/commissioning behavior, which still requires reviewed scheduling/ownership and authorization design. M7P7B quantitative current remains DEFERRED and GNSS coexistence remains unproven on the tested no-GNSS unit. ORUN application GATT, provisioning, secure RF, MESSAGE runtime and field-network/serviceability runtime remain later work; the separate intermittent serial-DFU issue is not closed by the successful UF2 recovery path.
Historical pre-M6 architecture baseline: `859ca4af0abf9f533a54227b38d2b1a5ddcfcccb`.


Merged security primitive evidence (M7P6C): M7P6C is a
test-only RAK4630 CryptoCell primitive proof, not a secure-RF implementation.
The RFC5869 HKDF-SHA256 / RFC3610 AES-128-CCM hardware KAT passed on owner
hardware. Independent review expanded the negative matrix and repeated-forgery
stress coverage; the expanded image also passed its aggregate hardware gate,
which requires all 13 mutation cases and all 1000 forged/valid recovery
iterations to succeed. The pinned CC310 binary reports `CRYS_FATAL_ERROR` for the observed
wrong-tag decrypt path; this is a narrow compatibility observation, not a
general production authentication-error classification. Production
CryptoCell/Bluefruit ownership and SoftDevice concurrency remain unresolved
gates for the later secure-envelope milestone. See
`docs/milestones/M7P6C.md`.

Merged security pre-wire candidate (M7P6D): M7P6D is a
documentation-only pre-wire **candidate contract** for direction-separated HKDF
traffic keys, 13-byte AES-CCM nonce construction, replay ownership/power-cut
semantics and the Bluefruit/CryptoCell lifecycle boundary. Independent security
review found no BLOCKER and its HIGH/MEDIUM documentation findings are resolved.
M7P6E plus the corrected fresh-pairing follow-up have since closed the
candidate ORUN host-vector, matching RAK KAT and scoped
Bluefruit/SoftDevice/CC310 coexistence gates. The remaining mandatory
device-side pre-wire foundation is durable fail-closed A2D replay-HWM
persistence; final v2 header/AAD/MTU bytes and provisioning authority/key
custody are still deliberately unfrozen. It does not allocate v2 bytes or
change production runtime. See `docs/milestones/M7P6D.md`.

Merged test-only security coexistence probe (M7P6E):
M7P6E uses the full production source graph with fixed public M7P6D candidate
KDF/nonce KAT material and adds no production secure-RF path. Host/build,
hardware boot KAT, bonded BLE connection/security update and direct LoRa RX
evidence passed. The stronger fresh-pairing LESC stress was historically
owner-waived, then hardened; the corrected-code rerun is now **PASS for the
scoped pinned RAK4631/framework/probe path**. This closes that specific
coexistence evidence gate, not arbitrary CryptoCell thread-safety or production
secure-envelope readiness. See `docs/milestones/M7P6E.md`.

Merged M7P7C application-boundary design:
`docs/milestones/M7P7C.md` records only the design-level BLE application/commissioning
boundary. It adds no GATT service, provisioning path, secure-RF bytes, MESSAGE runtime
or Android/backend code. Exact GATT framing and the commissioning ceremony remain later
implementation gates.

Merged M7P7D application request seam:
`docs/milestones/M7P7D.md` records the first fixed-memory, loop-owned,
transport-neutral application request/result seam and the read-only USB
`APP CONFIG?` adapter. It adds no protected writes, BLE application GATT,
provisioning, authorization, MESSAGE, commands or new wire bytes. Its external
independent review returned **PASS WITH FIXES**; accepted findings and final
revalidation are recorded in
`docs/audits/M7P7D_EXTERNAL_REVIEW_DISPOSITION.md`.

Merged M7P7E requester ownership (PR #34,
`main@cb1e181f88ed8d8362f6d4d2f97b96734474c954`) adds only explicit
requester/response ownership to the M7P7D seam before a second transport adapter
exists: requester-qualified take/discard, fail-closed invalid-requester rejection,
one bounded global response slot and request-ID namespaces per adapter.
Independent review returned **PASS WITH FIXES** with no BLOCKER/HIGH/MEDIUM
findings; accepted fixes and final revalidation are recorded in
`docs/audits/M7P7E_EXTERNAL_REVIEW_DISPOSITION.md`. It adds no BLE GATT, wire
format, provisioning, authorization, storage or RF behavior. See
`docs/milestones/M7P7E.md`.

M7P7F BLE application transport contract (branch
`feat/m7p7f-ble-app-transport-contract`,
baseline `main@f833a6d56de17b902bc26061a3791086a14f1cf4`) freezes the first
ORUN application transport wire contract -- three 128-bit UUIDs, an 8-byte
header / 20-byte-frame / 48-byte-logical-payload / 4-fragment framing rule,
`GET_CONFIG` request/response and `ERROR` byte layouts -- and adds a
Bluefruit-free, loop-owned `BleApplicationTransport` bounded
reassembly/session/backpressure component. Independent follow-up review found
two MEDIUM session/backpressure gaps plus one LOW wrap/active-state gap; all
were fixed, and post-fix owner revalidation on code-bearing head
`3ebb1b97f7e86bfbf502219619d90425a5f9686f` passed the full host
warnings-as-errors + ASan/UBSan suite, all 8 startup scenarios and the production
RAK4630 build. It adds no Bluefruit `BLEService`/`BLECharacteristic`, does
not change advertising/admission/bond behavior, and is not referenced anywhere
in production `main.cpp` composition; the corrected build remains exactly
22,124 B RAM / 226,292 B Flash, a **0 B / 0 B** delta versus merged M7P7E.
See
`docs/milestones/M7P7F.md`; exact GATT wiring, indication delivery and
physical phone validation remain M7P7G.

Owner-approved later application direction is recorded separately in
`ORUN_APP_ENTITY_MESSAGING_DIRECTION.md`: Entity Registry ownership/offline conflict
principles, person-location privacy, concise shared-map semantics, MESSAGE recipient/
DELIVERED semantics, Internet-first/LoRa-fallback/store-forward direction, presence
semantics and LoRa MESSAGE prerequisites. It is documentation-only and must not be read
as evidence that those runtimes exist.

The owner-provided independent review and ORUN disposition are recorded in
`docs/audits/M7P7C_INDEPENDENT_ARCH_REVIEW_DISPOSITION.md`.

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

Its application/UI wording is also superseded where the newer application-direction
record differs: MESSAGE v1 no longer requests a human read receipt; map
animal/person/vehicle category/icon is Entity Registry metadata rather than a firmware
identity; and real-world assignment history follows the current Entity Registry/binding
direction. Keep the historical file unchanged rather than rewriting it to look as if it
originally made those later decisions.

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
