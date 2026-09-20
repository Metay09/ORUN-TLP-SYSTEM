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

## 6. Future MESSAGE requirements preserved by this boundary

MESSAGE is a separate application service, not a BLE feature and not a LoRa packet type by itself.

A stable logical `message_id` survives transport changes/retries. Internet, BLE, LoRa and gateway custody are possible transports for the same logical message.

Owner-approved delivery policy for the future product:

1. when the recipient ORUN app has a current authenticated backend reachability/session indication, use the Internet delivery path and do **not** send the same message over LoRa in parallel;
2. when recipient Internet delivery is unavailable and a current ORUN/LoRa path exists, use the gateway/LoRa path;
3. when neither path exists, retain the message under bounded store-and-forward policy until a route appears or the message expires;
4. a later fallback/retry keeps the same `message_id` so duplicates can be suppressed;
5. `TX_DONE`, backend custody or gateway custody are not `DELIVERED`;
6. `DELIVERED` requires an authenticated recipient-endpoint acceptance/receipt;
7. MESSAGE v1 does not require a read receipt.

Internet reachability is not inferred from a Wi-Fi/mobile-data icon. It is based on an authenticated ORUN app/backend session, check-in or bounded lease whose exact mobile implementation remains M8 work.

Private MESSAGE content retains end-to-end protection. Gateway/relay infrastructure is transport/custody by default and must not become the message-decryption or user-authorization authority merely because it carries the traffic.

LoRa MESSAGE traffic must be bounded and lower priority than safety-critical/live operational traffic so chat backlog cannot starve alarms, critical command results or current tracking.

## 7. Optional user location sharing

Future person/user location is opt-in application data and is separate from device identity and from a tracker hardware/location-source decision.

When a user enables sharing:

- freshness/age is explicit;
- stale last-known data is never presented as live;
- authorization determines who may see it;
- disabling sharing stops new live updates under the later retention/privacy policy.

The application may render animals, people, vehicles, gateways, actuators/valves and sensors on the same map. That map entity/category is UI/application metadata; it must not be encoded as or inferred from the legacy firmware Role enum.

The normal map remains concise. Route/history, actuator operation history and sensor time-series belong to entity detail views rather than permanent map overlays. Exact Android UI is M8 scope.

### 7.1 Entity ownership / real-world binding

The device is not the canonical owner of what real-world thing it represents.

Future product ownership is:

```text
Device
  = technical identity, capabilities, enabled services, health and observations

Entity Registry
  = real-world binding, display name, category, permissions and UI metadata
```

The backend is the canonical owner of the Entity Registry. Authorized phones/gateways keep a bounded offline cache so local maps remain meaningful without Internet.

Examples of entity categories include ANIMAL, PERSON, VEHICLE, GATEWAY, ACTUATOR/VALVE and SENSOR. These categories are application/UI metadata and must not be encoded into the legacy firmware Role enum.

A physical ORUN device may be rebound to a different real-world entity over its lifetime. That binding must therefore be versioned/time-aware so historical observations remain attributed to the entity that owned the device at the observation time.

A device should report technical facts it truly owns (for example, actuation capability or a temperature sensor capability), but should not repeatedly transmit UI metadata such as "cow", display name or emoji over LoRa.

Person/user location remains user/application data and does not require a dedicated ORUN hardware device.

## 8. What the first implementation slice must prove

Before protected application GATT writes are enabled, the implementation milestone must:

1. define exact GATT UUIDs/framing and bounded MTU/fragmentation behavior;
2. classify which bootstrap fields, if any, are safe before application authorization;
3. define the commissioning/authentication ceremony rather than relying on stock Just Works bonding;
4. ensure GATT callbacks only hand off bounded work to the ORUN owner loop/task;
5. route config operations to the existing configuration owner rather than direct flash writes;
6. keep SecurityStore root material non-readable through generic APIs;
7. host-test malformed length/state/disconnect/replay-like duplicate request handling;
8. build the unchanged RAK4630 production graph and measure RAM/flash delta;
9. physically verify GATT lifecycle with a phone only after the above contract exists.

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
