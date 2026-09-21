# M7P7C — BLE application / commissioning boundary

Status: **DESIGN-ONLY OWNER DIRECTION; NO PRODUCTION RUNTIME, GATT, WIRE, STORAGE OR SECURITY-CREDENTIAL CHANGE.**

Baseline: `main@46d7a933f63d42d84fb386035be1c03af1d0c3c4` (M7P6E merged; production firmware restored and M7P7B BLE runtime already on main).
Branch: `docs/m7p7c-ble-application-contract`.

## 1. Why this slice exists

M7P7B proved the production Bluefruit/SoftDevice runtime and BLE admission lifecycle, but deliberately added no ORUN-specific application GATT, provisioning, ownership or authorization service.

The security ADR also deliberately leaves the exact provisioning ceremony unfrozen. It explicitly forbids treating BLE bonding as ORUN authorization or exporting `K_root` over BLE/USB merely because a transport is locally reachable.

Therefore the next safe step is to freeze the application/ownership boundary before introducing a GATT surface that Android, messaging, configuration and later command/control would have to live with.

This slice is documentation only. It does not add a BLE service.

## 2. Current facts that remain unchanged

- `Bluefruit.begin()` is already production runtime.
- TRACKER BLE admission remains the M7P7B ~10-minute no-client policy.
- The current framework bond is transport security state only; it is **not** ORUN user/application authorization.
- `SecurityStore` has no ordinary root-key readback API and production has no credential provisioning call path.
- `ConfigStore` remains the durable owner of the currently implemented config fields.
- TLP v1 bytes, one-hop RELAY_FORWARD behavior and all compatibility fixtures remain frozen.
- The M7P6E fresh-pairing LESC/CC310 coexistence stress remains owner-waived/not-PASS; the production secure-envelope concurrency gate therefore remains open.

## 3. BLE application boundary

BLE is a **transport adapter**, not a new application owner.

Future BLE application traffic must hand requests to the same service/configuration/command owners that USB, LoRa or another transport would use. Do not build independent BLE-only business logic or a second configuration system.

Conceptually:

```text
Android / local client
        |
       BLE
        |
ORUN application transport adapter
        |
        +--> configuration owner
        +--> diagnostics/status owner
        +--> MESSAGE service
        +--> command/key-access service
        +--> gateway bridge
```

The GATT layer must not infer Role, capability, user identity, authorization or location ownership from the fact that a phone connected.

Exact service UUIDs, characteristic byte layouts and fragmentation are **not frozen by M7P7C**. They belong to the first implementation slice after its authorization/commissioning contract is reviewed.

## 4. Callback and concurrency ownership

A future Bluefruit GATT callback must remain bounded.

Allowed callback work is limited to validating basic bounds and copying/enqueuing a bounded request or recording a small event token. It must not perform:

- flash mutation;
- CryptoCell operations;
- radio TX/RX state transitions;
- blocking waits;
- application command execution;
- configuration commits;
- Serial-heavy diagnostics.

Loop/task-owned ORUN code performs the actual work and owns retries, timeouts and results. This preserves the M7P7A/M7P7B flash/event ownership model and avoids introducing a new cross-task owner.

## 5. Authorization and commissioning boundary

These remain separate:

```text
BLE connection
!= BLE bond
!= ORUN device credential
!= user identity
!= application authorization
```

A bonded nearby phone must not automatically gain access to private location, messages, configuration mutation or command/control.

For the first commissioning design:

- first credentials require explicit provisioning;
- ordinary config reset does not clear security credentials;
- re-provisioning creates a new credential lifetime/`credential_id`;
- security reset/re-provision remains a separately authorized physical-service operation;
- there is no generic `K_root` readback;
- keys do not appear in ordinary logs, diagnostics or config dumps.

M7P7C does **not** choose or invent the final bootstrap cryptographic ceremony. QR/claim-secret, USB possession, BLE OOB, backend registration, key wrapping/escrow and additional-phone delegation require the focused provisioning implementation review. No production credential write path is authorized by this document alone.

That provisioning review must also decide authority-key custody, backup/recovery
and disaster-recovery semantics **before production credentials are written**.
This does not mean the backend is automatically required to store raw
`K_root`: the current security ADR deliberately leaves backend registration,
key wrapping/escrow and authority placement unfrozen. Whatever design is chosen
must preserve the per-device compromise boundary and avoid a single fleet/group
authority key.

The M7P6E fresh-pairing LESC/CC310 coexistence stress remains owner-waived/not-
PASS. Because commissioning is a fresh-pairing-sensitive path, its reviewed
closure (or an explicitly safe serialization/backend strategy) is a merge gate
for production commissioning, not for this documentation-only M7P7C slice.

## 6. Application/entity/messaging decisions are separate from this BLE slice

M7P7C does not own the future MESSAGE routing policy, Entity Registry schema,
person-location privacy or map UX. Those owner-approved product/application
directions are recorded separately in
`docs/architecture/ORUN_APP_ENTITY_MESSAGING_DIRECTION.md`.

The BLE consequence is narrow: future MESSAGE/entity/config/command traffic may
use BLE as a transport, but BLE connection/bonding does not become the
application owner, recipient identity, Entity Registry or authorization source.

## 8. What the first implementation slice must prove

Before protected application GATT writes are enabled, the implementation milestone must:

1. define a small transport-neutral bounded request/result seam before freezing BLE-specific business semantics; USB/host injection may be used to test that seam without making USB a second application owner;
2. define exact GATT UUIDs/framing and bounded MTU/fragmentation behavior on top of that seam;
3. classify which bootstrap fields, if any, are safe before application authorization;
4. define the commissioning/authentication ceremony rather than relying on stock Just Works bonding;
5. ensure GATT callbacks only hand off bounded work to the ORUN owner loop/task;
6. bound per-connection request/fragment queues, define overflow/rate-limit behavior and discard incomplete fragments on disconnect;
7. route config operations to the existing configuration owner rather than direct flash writes;
8. keep SecurityStore root material non-readable through generic APIs;
9. host-test malformed length/state/disconnect/duplicate/flood handling;
10. test whether unauthenticated/Just Works pairing can exhaust the finite bond store or otherwise deny later legitimate commissioning, and ensure application authorization does not depend on bond-table presence alone;
11. build the unchanged RAK4630 production graph and measure RAM/flash delta;
12. physically verify GATT lifecycle with a phone only after the above contract exists.

Protected secure-RF/message/command traffic remains separately gated on the reviewed secure-envelope implementation and its open CC310/Bluefruit coexistence/concurrency requirement.

## 9. Compatibility / system impact

This documentation slice changes no runtime behavior.

```text
TLP v1 bytes/sizes:             unchanged
RF PHY/airtime:                unchanged
relay semantics:               unchanged
History/Config/Security bytes: unchanged
BLE runtime/admission:         unchanged
BLE application GATT:          not implemented
provisioning:                  not implemented
DFU:                           not implemented
secure RF envelope:            not implemented
MESSAGE runtime:               not implemented
user-location runtime:         not implemented
Android/backend:               not implemented
```

No host/build/hardware PASS is claimed or required for this docs-only slice.
