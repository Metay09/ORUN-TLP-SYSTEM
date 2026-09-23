# ORUN Architecture Gap Analysis

Audited main: `aa3bbf810a37034d9a3d9066ede4bf579646adfe`.
Date: 2026-09-14. Documentation only; no finding below was implemented here.
Target rationale and cross-cutting contracts are in
[System Architecture V1](ORUN_SYSTEM_ARCHITECTURE_V1.md); byte compatibility is in
[Protocol Evolution Plan](ORUN_PROTOCOL_EVOLUTION_PLAN.md).
Evidence paths are relative to the repository root. Symbol names identify the
reviewed code without relying on line numbers that will shift in later work.

## 2026-09-23 coverage update

This section updates implementation coverage without rewriting the historical
G01-G24 finding bodies below. Those finding bodies remain the audit record for
`main@aa3bbf810a37034d9a3d9066ede4bf579646adfe` on 2026-09-14.

Coverage was reviewed against merged `main@9522e391a532f21ef76ee092889d591cb1c2cf78`
(M7P7F / PR #35) plus the active, **not-yet-merged**
`feat/m7p7g-ble-app-gatt@253e7ea927b24f1f5cb01e50f45304d1cd5323b6`.
An active-branch result is not merged product behavior and must not be cited as
`main` evidence. Physical evidence also remains tied to the exact code-bearing
head recorded by each milestone.

Status meanings:

- **CLOSED:** the original gap's scoped architectural defect is removed in the
  current merged design/production path; later product extensions may still exist.
- **FOUNDATION CLOSED:** the dangerous ownership/architecture gap is closed, but
  the complete user-facing feature intentionally remains later work.
- **PARTIAL:** meaningful implementation exists, but a required product/runtime
  portion of the original target remains open.
- **OPEN:** the required foundation/runtime does not yet exist and gates dependent
  work.
- **DEFERRED:** intentionally not required until its recorded dependency/use case.

| Gap | 2026-09-23 status | Current coverage / remaining boundary |
| --- | --- | --- |
| G01 Role conflates application and forwarding | **FOUNDATION CLOSED** | B3/B4 separate relay forwarding from legacy role and allow tracking + relay to be represented. Production requested intent still comes from the legacy compatibility projection until a reviewed persistent configuration surface replaces it. |
| G02 AUTO infers responsibility from GNSS | **FOUNDATION CLOSED** | AUTO is preserved only as the unprovisioned legacy bootstrap. Requested/capability/effective state is separated, but explicit persisted provisioning/config precedence is not yet production-owned. |
| G03 POSITION encoder coupled to GnssFix and radio | **CLOSED** | B2 introduced portable `GnssFix`, independent `DeviceIdentity` and pure legacy POSITION mapping. Production PositionFlow no longer depends on RadioManager for POSITION construction. |
| G04 SparkFun dependency in public GNSS header | **FOUNDATION CLOSED** | Portable consumers can use `gnss_fix.h` without SparkFun. The concrete u-blox implementation remains inside GnssManager; broader driver extraction is intentionally deferred until a real second platform requires it. |
| G05 Identity owned by radio initialization | **CLOSED** | Production resolves RAK DeviceIdentity before radio startup and injects it into radio/history/position ownership. Existing legacy IDs remain byte-stable. |
| G06 Flash contract mixes platform declaration and geometry | **DEFERRED** | Current RAK flash ownership is explicit and guarded. Further board-contract cleanup is due only when a real second platform demonstrates the need. |
| G07 New persistence and BLE need a verified partition plan | **FOUNDATION CLOSED** | M7P1-P7 allocate and enforce separate SecurityStore, ConfigStore, relocated bond/InternalFS and HistoryStore ownership with SoftDevice-aware flash arbitration. DFU preservation/rollback and broader physical power-cut evidence remain open. |
| G08 No general configuration persistence | **PARTIAL** | M7P5 ConfigStore durably owns `tracking_interval_seconds` and `battery_capacity_mah`. General profile/service/relay/location/RF/geofence configuration migration and revisioned whole-candidate persistence remain later work. |
| G09 No transport-independent command/config boundary | **PARTIAL** | M7P7D/E provide a fixed-memory ApplicationRequestService/requester boundary; USB and M7P7G BLE both adapt into it for read-only GET_CONFIG. Authenticated issuer context and protected writes/commands remain security-gated. |
| G10 Location source ownership and durable last-known missing | **PARTIAL** | GNSS is separated from generic Location semantics and M6C3 accepts source-neutral accepted positions. PHONE/MANUAL ownership, active-source generation, persistent last-valid point and product freshness state remain unimplemented. |
| G11 NODE_LOCATION and movement suspicion absent | **DEFERRED** | Still future work, to follow explicit location-source and protocol evolution decisions. |
| G12 EVENT transport and authenticated contact absent | **OPEN** | No authenticated EVENT/receipt/contact runtime exists. This still blocks trustworthy network-contact LOST and critical-event delivery claims. |
| G13 Compact heterogeneous telemetry absent | **DEFERRED** | No generic telemetry family is needed until the first non-position telemetry service is implemented. |
| G14 Relay envelope and network events are POSITION-specific | **PARTIAL** | TLP v1 remains intentionally frozen for development compatibility. Current architecture now directs new protected multi-service traffic to an explicit secure TLP v2 path; the generic authenticated runtime envelope is not yet implemented. |
| G15 Command execution and actuator safety not modeled in code | **DEFERRED** | No actuation product path is authorized before security, idempotency, expiry, result and physical fail-safe gates. |
| G16 Human messaging, user identity and mailbox absent | **DEFERRED** | Entity/MESSAGE ownership and routing direction are documented, but no MESSAGE runtime/backend/app mailbox exists. |
| G17 No shared traffic admission/QoS policy | **PARTIAL** | Existing position/relay paths are bounded and tracker RX availability has been made explicit, but no common multi-class airtime scheduler exists for events/backlog/messages/commands. |
| G18 Security and replay lifecycle missing | **PARTIAL — BLOCKING DEPENDENTS** | SecurityStore, TX nonce persistence, CryptoCell KATs, secure-envelope pre-wire design and scoped CryptoCell/Bluefruit coexistence evidence exist. Production commissioning, secure TLP v2 envelope, RX replay state, application authorization and key/authority lifecycle are still open. |
| G19 Hardware adaptation still concentrated in managers | **DEFERRED** | RAK4630/RAK4631 remains the owned reference platform. No speculative HAL is justified before a second real platform exists. |
| G20 GNSS power modes and shared sensor resources need evidence | **PARTIAL** | Sensor rail ownership, bounded recovery, accelerometer integration and tracker post-TX RX window behavior are implemented. Quantitative system current, GNSS backup/power-save evidence and full energy policy remain unmeasured/unimplemented. |
| G21 History retention and delivery are bounded foundations | **PARTIAL** | Store-before-send history and delivery/replay ownership seams exist. Authenticated receipts, custody semantics and automatic live-first backlog replay remain absent. |
| G22 Backend/UI/gateway semantics not implemented | **DEFERRED / OPEN FOR M8** | Offline/local gateway, Entity Registry, truthful freshness and app/backend semantics are documented only; no production Android/backend/gateway bridge exists. |
| G23 Test/version/DFU contracts need incremental expansion | **PARTIAL** | Golden compatibility, startup/fault tests, sanitizer/warnings gates, storage guards and multiple focused hardware gates are substantially stronger. Product DFU preservation, rollback/authenticity and schema migration evidence remain open. |
| G24 Physical validation incomplete | **PARTIAL — ONGOING** | Focused physical evidence now exists for direct GNSS→POSITION, accelerometer diagnostics, tracker RX-window behavior, BLE lifecycle/bond/LoRa coexistence and normal M7P7G GATT on its recorded head. Quantitative power, long-range/scale RF, enclosure/mechanics, destructive power-cut matrices and several latest-head regressions remain open. |
| G25 Battery/energy observability and power-health policy | **OPEN — BEFORE FIELD PILOT** | Battery capacity can be stored, but production battery voltage/SOC/charge/solar state, low/critical thresholds, user-visible power health and measured runtime budget are not implemented. See the new finding below. |

### Coverage conclusions

1. G03 and G05 are closed at their original architectural scope.
2. G01/G02/G04/G07 have their dangerous foundation gaps closed without claiming
   that the final product feature set is complete.
3. G12 and the remaining G18 secure-runtime work are the principal blockers for
   trusted EVENT/LOST, remote configuration, OPEN_BLE, COMMAND/RESULT, private
   location and MESSAGE.
4. G08/G09/G10/G17/G21 are intentionally partial foundations and should be
   extended only when their next real product consumer arrives.
5. G06/G11/G13/G15/G16/G19/G22 remain intentionally deferred rather than
   accidental omissions.
6. G24 is not a one-time task: every hardware-sensitive milestone keeps its own
   exact physical evidence boundary.
7. G25 is newly registered because energy/battery behavior was present in the
   system architecture but had no explicit gap-register owner. It does not block
   M7P7G closure or the secure-envelope foundation, but it must close to an
   appropriate field-pilot level before claiming deployable animal-tracker
   battery health or endurance.

## Priority and timing

- **P0:** current unsafe behavior or blocker requiring immediate correction to
  continue the presently authorized prototype work.
- **P1:** fix at the stated gate before M6/M7 or before costly semantic lock-in.
- **P2:** important, safely deferred until its specified dependency/use case.
- **P3:** future-only consideration; not a mandate for pre-M6 implementation.

**P0: NONE confirmed by this architecture audit at the frozen baseline.** This is
not a certification that firmware has no defects. Current unauthenticated v1 is
unsuitable for protected deployments; BLE mounting the history region would be
unsafe if introduced, but is not a current execution path. Security and storage
are explicit stop gates before enabling those future functions.

Timing labels: immediately required; pre-M6 required; pre-M7 required;
pre-security required; before second hardware platform; future only.
“Wire: none” and “Storage: none” refer to the smallest migration, not every
future capability described in the target.

## Findings

### G01 — Role conflates application and forwarding (P1; expected area A)

| Field | Assessment |
| --- | --- |
| Current state | TRACKER gates fix consumption/POSITION; RELAY forwards; BASE receives unique application data. GNSS polling and continuous RX remain independent of this shorthand. |
| Evidence | `firmware/src/main.cpp:loop`, `firmware/src/node_role.cpp`, `firmware/src/network_service.cpp:receive` |
| Risk | Adding SENSOR/PERSON/ACTUATOR roles grows coupled branches and invalid hardware assumptions. |
| Target state | END_NODE/RELAY forwarding responsibility; independent gateway bridge, application preset, capability, source and power policy. |
| Smallest change | Introduce explicit compatibility mapping and pure config values; keep legacy commands and effective behavior. |
| Dependencies | G02, G08/G09 validation rules; regression fixtures. |
| Compatibility impact | Legacy names accepted; BASE mapping does not invent a BLE/IP bridge; no implicit GNSS/RX changes. |
| Wire impact | None; role is not a current wire field. |
| Storage impact | None for mapping; later persisted role/profile fields use a new config schema. |
| Tests | Every legacy role, requested vs installed transition, tracker never forwards, GNSS-capable BASE/RELAY and absent-GNSS tracker. |
| Recommended milestone | Pre-M6 required, before adding application branches. |

### G02 — AUTO infers responsibility from GNSS (P1; B)

| Field | Assessment |
| --- | --- |
| Current state | Detection completion selects TRACKER if detected, otherwise BASE; override lasts until reboot. |
| Evidence | `firmware/src/node_role.cpp:RoleController::updateAutomatic`, `firmware/src/main.cpp:automatic_role_resolved` |
| Risk | A missing sensor can change topology/application identity; future saved intent could be overwritten. |
| Target state | AUTO is legacy unprovisioned preset suggestion; explicit config always wins and capability failure only degrades the affected service. |
| Smallest change | Preserve current bootstrap behind compatibility mode; define config precedence before adding persistence. |
| Dependencies | G01, G08, G09. |
| Compatibility impact | Existing unconfigured nodes keep exact bootstrap until an explicit migration; no automatic change to deployed roles. |
| Wire impact | None. |
| Storage impact | Future provisioned marker/schema; never infer it from history contents. |
| Tests | Saved config with GNSS present/absent/faulting, boot overrides, detection retries and repeated boots. |
| Recommended milestone | Pre-M6 required for separation; persistence precedence before persistent provisioning. |

### G03 — POSITION encoder coupled to GnssFix and radio (P1; C)

| Field | Assessment |
| --- | --- |
| Current state | PositionFlow takes GnssFix and asks RadioManager to encode/allocate identity before append. |
| Evidence | `firmware/include/position_flow.h`, `firmware/src/position_flow.cpp:acceptFix`, `firmware/src/radio_manager.cpp:encodePosition` |
| Risk | New location sources may fabricate GNSS metadata; radio refactoring can accidentally break local history during RF failure. |
| Target state | Neutral observation/data header and pure legacy POSITION mapping; transport sees bytes. |
| Smallest change | Extract value/mapping boundary without replacing PositionFlow or moving persistence/freshness gates. |
| Dependencies | G04/G05; independent golden vectors and startup failure suite. |
| Compatibility impact | Same GNSS admission, sequence use, store-before-TX and five-second live expiry. PHONE is not mapped to legacy GNSS POSITION. |
| Wire impact | None; byte-identical 34-byte packets. |
| Storage impact | None; preserve v3 records and sequence tickets. |
| Tests | Golden bytes, store/readback before send, stale while appending, RF startup failure still stores locally. |
| Recommended milestone | Pre-M6 required before source-neutral location consumers. |

### G04 — SparkFun dependency in public GNSS header (P1; D)

| Field | Assessment |
| --- | --- |
| Current state | `gnss_manager.h` includes SparkFun and declares private UBX callbacks; neutral GnssFix shares that header. |
| Evidence | `firmware/include/gnss_manager.h`, `firmware/src/gnss_manager.cpp` |
| Risk | Consumers transitively require a concrete library; later driver replacement leaks into core. |
| Target state | Source-neutral value header; concrete UBX parsing stays within RAK/u-blox implementation. |
| Smallest change | Move the portable value declaration first; hide callback/library declarations only with a small supported seam. Do not rewrite GNSS state machine. |
| Dependencies | G03; M3/R3/R4 tests. |
| Compatibility impact | Preserve PVT+DOP/session/queue-drain/UTC semantics; no generic GNSS driver framework now. |
| Wire impact | None. |
| Storage impact | None. |
| Tests | Compile neutral consumers without SparkFun; run delayed-pair, partial-drain and bounded recovery regressions. |
| Recommended milestone | Pre-M6 required for value header; remaining driver extraction before second hardware platform. |

### G05 — Identity owned by radio initialization (P1; E)

| Field | Assessment |
| --- | --- |
| Current state | nRF factory bytes captured at the start of RadioManager::begin, before failure returns; startup safety already repaired. |
| Evidence | `firmware/src/radio_manager.cpp:begin`, `docs/audits/PRE_M6_INTEGRATION_SAFETY.md`, pinned SX126x `boards/mcu/nrf52832/board.cpp` |
| Risk | Future radio/MCU change could alter device IDs or recreate history-loss startup ordering. |
| Target state | Opaque DeviceIdentity available independently of radio; legacy mapping stable; future provisioning/conflict policy. |
| Smallest change | Small identity provider with existing exact algorithm and early availability; no new ID allocation. |
| Dependencies | Startup suite and legacy ID vectors; G18 before keys bind to identity. |
| Compatibility impact | Existing devices, database keys and log formatting remain identical. |
| Wire impact | None, existing uint64 values unchanged. |
| Storage impact | No page identity migration; config reset must not reset identity/security generation. |
| Tests | Identity on mutex/gate/queue/LoRa failure, byte ordering, high/low halves, leading-zero logs; later clone enrollment rejection. |
| Recommended milestone | Pre-M6 required boundary; new namespace decision pre-security/before second platform. |

### G06 — Flash contract mixes platform declaration and geometry (P2; F)

| Field | Assessment |
| --- | --- |
| Current state | Portable virtual FlashBackend exists; NrfHistoryFlash declared in same header; storage constants include nRF address and record geometry. |
| Evidence | `firmware/include/flash_backend.h`, `firmware/include/storage_config.h`, `firmware/src/nrf_history_flash.cpp` |
| Risk | Another backend may inherit wrong physical assumptions; needless rewrite could discard R1 durability. |
| Target state | Separate board layout/concrete declaration from journal format and durable flash contract. |
| Smallest change | Split declarations when config layout or second board needs them; keep actual backend algorithms. |
| Dependencies | G07 partition proof; R1 backend contract. |
| Compatibility impact | No behavior change required for a header split. |
| Wire impact | None. |
| Storage impact | None until separately approved layout change; do not silently change geometry. |
| Tests | Existing production nRF backend tests, alignment/bounds/readback and SoftDevice refusal; later board contracts. |
| Recommended milestone | Before second hardware platform; earlier only if G07 requires it. |

### G07 — New persistence and BLE need a verified partition plan (P1)

| Field | Assessment |
| --- | --- |
| Current state | History exclusively occupies 0xED000..0xF4000; no general config partition. SoftDevice-enabled flash is refused; Bluefruit startup mounts overlapping InternalFS. |
| Evidence | `docs/storage/M4_FLASH_JOURNAL.md`, `firmware/scripts/check_storage_layout.py`, `nrf_history_flash.cpp`; installed core linker, InternalFileSystem.cpp, Bluefruit `bond_init` |
| Risk | Naive BLE/config integration overwrites history or disables store-first transmissions; adjacent flash assumptions can corrupt boot/update regions. |
| Target state | Proven partition ownership, separate record/reset policies, SoftDevice-safe completion and bond storage, DFU growth budget. |
| Smallest change | Audit physical bootloader/SoftDevice/upload layout and approve page budget. Evaluate reducing application ceiling or external storage in a separate task; allocate no page here. |
| Dependencies | Actual board readout and bootloader artifacts, capacity/retention decision; G08/G10/G18 workload estimates. |
| Compatibility impact | Keep current enabled-mode and exclusive-owner guards; future updates require migration/preservation plan. |
| Wire impact | None. |
| Storage impact | Future explicit new partitions/formats; seven history pages retained unless a separately approved migration proves otherwise. |
| Tests | Boundary guards, old records after update, cut at every config commit/migration step, corrupt schema, BLE bond/history coexistence and real flash power cuts. |
| Recommended milestone | Layout decision pre-M6 required before durable additions; asynchronous backend/bond/DFU integration pre-M7 required. |

### G08 — No general configuration persistence (P1; H)

| Field | Assessment |
| --- | --- |
| Current state | RF/GNSS defaults compile-time; role override RAM only. History is not a config database. |
| Evidence | `firmware/include/{radio_config,gnss_config}.h`, `firmware/src/main.cpp:handleRoleCommand` |
| Risk | Settings disappear at boot; future independent knobs become inconsistent or unsafe across upgrades. |
| Target state | Versioned schema, whole-candidate validation, revisioned atomic commit, explicit reset scopes. |
| Smallest change | Define/implement portable validation and existing-default config snapshot first; add persistence only after G07. |
| Dependencies | G01/G02/G07/G09. |
| Compatibility impact | Preserve defaults including RF; distinguish requested/persisted/applied state; no silent reprofiling. |
| Wire impact | None for USB-only path; later radio config commands depend on G18/G14. |
| Storage impact | New bounded config snapshots, version/migration and wear policy, not journal format reuse. |
| Tests | Invalid partial updates leave old config, cross-field validation, revisions, unknown schemas, power loss/reset/downgrade. |
| Recommended milestone | Pre-M6 required boundary; persistence before persistent features, not necessarily before isolated accelerometer experiments. |

### G09 — No transport-independent command/config boundary (P1; I)

| Field | Assessment |
| --- | --- |
| Current state | Bounded USB parsing and application of role commands live in main.cpp; no privileged radio/BLE path. |
| Evidence | `firmware/src/main.cpp:pollRoleCommands/handleRoleCommand`, `firmware/src/node_role.cpp:parseRoleCommand` |
| Risk | BLE/radio handlers could duplicate validation, privilege and persistence rules. |
| Target state | Framing adapters deliver typed requests with issuer/auth context to one validator/handler and result path. |
| Smallest change | Route existing USB ROLE semantics through a small pure handler, retaining bounds and quiescent radio application. |
| Dependencies | G08; G18 before remote privileged ingress. |
| Compatibility impact | Existing USB syntax/results retained; no new remote command functionality. |
| Wire impact | None. |
| Storage impact | None initially; Config owner handles later durability. |
| Tests | 24-byte buffer/32-byte loop budget, malformed commands, requested vs installed roles, no mutation on rejection. |
| Recommended milestone | Pre-M6 required boundary; authorization pre-security and pre-M7 required. |

### G10 — Location source ownership and durable last-known missing (P1; J)

| Field | Assessment |
| --- | --- |
| Current state | Only GNSS fix handoff exists; no PHONE/MANUAL/NONE active-owner or last-known store. |
| Evidence | `firmware/include/gnss_manager.h:GnssFix`, `firmware/src/main.cpp:loop`, absence of other location services in `firmware/src` |
| Risk | A future disconnect/empty input erases location, or GNSS silently overwrites PHONE ownership; stale data appears live. |
| Target state | Source-neutral values, explicit singular owner/generation, persistent last-valid and separate freshness/connectivity. |
| Smallest change | GNSS-only neutral handoff first; specify PHONE transition table now; add PHONE persistence separately after storage proof. |
| Dependencies | G03/G04/G07/G08/G09. |
| Compatibility impact | Current GNSS flow unchanged; source switch never relabels old data; explicit CLEAR only deletes. |
| Wire impact | None for neutral handoff; phone publication needs separate semantics, not legacy POSITION reuse. |
| Storage impact | Later versioned durable source/revision/timestamp/validity and clear tombstone. |
| Tests | Disconnect, silence, OS/permission off, empty/invalid input, GNSS while PHONE owner, mode off, reboot, 0/0, clock unknown, out-of-order sessions and clear recovery. |
| Recommended milestone | Pre-M6 required neutral ownership design; actual PHONE support optional later, pre-M7 if included in BLE scope. |

### G11 — NODE_LOCATION and movement suspicion absent (P2; K)

| Field | Assessment |
| --- | --- |
| Current state | v1 POSITION is GNSS/moving-fix oriented, with no location source/revision or fixed-node advertisement. |
| Evidence | `protocol/M2_POSITION_PACKET.md`, `firmware/include/tlp_position_packet.h` |
| Risk | Manual/phone positions acquire invented GNSS fields; repeated advertisements refresh stale points. |
| Target state | Separate NODE_LOCATION family and independent announcement settings; fixed-node movement retains point with warning. |
| Smallest change | Finalize semantics and measured payload/airtime budget before new codec; no packet ID assigned now. |
| Dependencies | G10/G14/G17 and later accelerometer evidence. |
| Compatibility impact | Legacy POSITION stays GNSS-only; older nodes cannot display/relay new family. |
| Wire impact | New type or v2 application family, never mutate 34-byte POSITION. |
| Storage impact | Durable last-known revision; periodic OFF does not erase it. |
| Tests | Boot jitter, on-change coalescing, OFF/repeat/publish-now, stale timestamp retention, moved warning and unknown age. |
| Recommended milestone | Future only, with phone/fixed-node location foundation. |

### G12 — EVENT transport and authenticated contact absent (P1; L)

| Field | Assessment |
| --- | --- |
| Current state | No event packet/ACK; TX_DONE never advances delivery state or proves network contact. |
| Evidence | `firmware/src/position_flow.cpp`, `firmware/src/network_service.cpp`, `docs/milestones/M4.md`, `docs/milestones/M5.md` |
| Risk | M6 LOST could use false contact evidence; alarm retries duplicate occurrences or starve live reports. |
| Target state | Stable durable event occurrence, severity/lifecycle, bounded authenticated receipt/retry and explicit contact semantics. |
| Smallest change | Implement local events only when M6 needs them; gate RF critical receipts/contact on security/protocol work. |
| Dependencies | G07/G14/G17/G18; local geofence quality rules. |
| Compatibility impact | Do not treat M5 reception log or local completion as remote receipt. |
| Wire impact | New EVENT/ACK family with reviewed compatibility. |
| Storage impact | Typed critical event retention separate from position ring; occurrence ID durable across retries/reboot. |
| Tests | Onset/update/clear, duplicate ACK/lost ACK, reboot, priority bounds, spoofed contact, OUTSIDE contact timeout with unavailable clock. |
| Recommended milestone | Pre-M6 required design; before networked geofence/LOST critical delivery within M6. |

### G13 — Compact heterogeneous telemetry absent (P2; M)

| Field | Assessment |
| --- | --- |
| Current state | POSITION and TEST only; no general metric registry or batching. |
| Evidence | `firmware/include/tlp_*packet.h`, `protocol/` |
| Risk | Sensor-specific protocol proliferation or oversized generic payloads. |
| Target state | Bounded metric batches, scaled integers, quality/time and unknown-field handling. |
| Smallest change | Benchmark fixed/TLV/schema/mixed options against representative sensor batches before final selection. |
| Dependencies | G14/G17; actual sensor data and MCU resource measurements. |
| Compatibility impact | Unknown metric skippable only with trustworthy length; no silent unit mismatch. |
| Wire impact | New TELEMETRY family; no final IDs/encoding selected. |
| Storage impact | New typed observation records/retention; no reuse of GNSS layout for unrelated sensors. |
| Tests | Signed/scaled boundaries, malformed lengths, unknown IDs, batching latency, worst-case size and old/new decoding. |
| Recommended milestone | Future only; before first non-position telemetry service. |

### G14 — Relay envelope and network events are POSITION-specific (P1; G)

| Field | Assessment |
| --- | --- |
| Current state | RelayForwardPacket embeds 34 POSITION bytes; NetworkEvent contains PositionPacket; unknown types are not forwarded. |
| Evidence | `firmware/include/{tlp_relay_forward_packet,network_service}.h`, `firmware/src/network_service.cpp:receive/takeDueForward` |
| Risk | EVENT/COMMAND/MESSAGE cannot traverse legacy relay; changing type 0x03 in place breaks deployed receivers. |
| Target state | Generic bounded authenticated envelope, opaque application payload, one RF relay hop and loop prevention. |
| Smallest change | Design v2 coexistence and independent golden vectors now; implement only when first new forwarded family is needed. |
| Dependencies | G17/G18 addressing/security; explicit downlink topology. |
| Compatibility impact | Legacy relay remains POSITION-only; upgrade relays/gateways before enabling new traffic. |
| Wire impact | Future new envelope/version, no mutation of existing 49-byte format. |
| Storage impact | None for forwarding-only RAM queue; persistent app IDs/mailbox require separate formats. |
| Tests | Legacy exact inner bytes; new malformed envelopes, hop/dedupe bounds, mixed fleet, opaque payload, replay and bridge loops. |
| Recommended milestone | Pre-M6 required design; pre-security required specification and before new relayed event/control traffic. |

### G15 — Command execution and actuator safety not modeled in code (P2; N)

| Field | Assessment |
| --- | --- |
| Current state | Only development ROLE commands; no physical output/control service. |
| Evidence | `firmware/src/main.cpp:handleRoleCommand`, `firmware/src/node_role.cpp` |
| Risk | Retries/reboot can repeat effects; UI equates send with valve state. |
| Target state | Authenticated scoped idempotent commands, expiry, receipt/result/feedback distinction and local fail-safe limits. |
| Smallest change | Specify command state machine and per-actuator safety case when control is commissioned; add no outputs now. |
| Dependencies | G07/G09/G14/G17/G18; actual actuator/feedback hardware and process owner. |
| Compatibility impact | No privileged downgrade to unauthenticated v1; no exactly-once physical promise. |
| Wire impact | New COMMAND/RESULT/ACK families under security envelope. |
| Storage impact | Durable idempotency/result ledger, recovery UNKNOWN state, bounded expiry. |
| Tests | Duplicate same/different arguments, expired execution, network loss, manual override, max-on duration, reset/power loss and feedback failure. |
| Recommended milestone | Future only; security/safety gates mandatory before actuation. |

### G16 — Human messaging, user identity and mailbox absent (P3; O)

| Field | Assessment |
| --- | --- |
| Current state | No human recipients, addressing, mailbox, fragmentation or app delivery protocol. |
| Evidence | `firmware/include/network_service.h`, `protocol/`, `docs/milestones/M5.md` |
| Risk | Overload, privacy leak, false delivery/read state or device ownership mistaken for user identity. |
| Target state | Short text/status/emergency service with separate user/device IDs, local offline path, optional IP bridge and E2E-compatible ciphertext transport. |
| Smallest change | Reserve semantic boundaries in design; no messaging implementation before prerequisites. |
| Dependencies | G07/G14/G17/G18/G22; destination rendezvous and endpoint provisioning. |
| Compatibility impact | Legacy relays do not carry messages; TX_DONE cannot mean delivered/read. |
| Wire impact | New MESSAGE/application receipts; bounded fragmentation only if justified by payload tests. |
| Storage impact | Quota/TTL-limited mailbox, durable custody vs RAM admission, privacy and deletion policy. |
| Tests | Unreachable recipient, expiry without UTC, lost receipts, restart, duplicate fragments, quota abuse, E2E relay opacity and gateway loops. |
| Recommended milestone | Future only, after security/QoS and endpoint transport. |

### G17 — No shared traffic admission/QoS policy (P1; P)

| Field | Assessment |
| --- | --- |
| Current state | PositionFlow live precedence, fixed relay queue, disabled TEST beacons; no multi-class airtime policy. |
| Evidence | `firmware/src/{main,position_flow,radio_manager,network_service}.cpp`, `firmware/include/{relay_config,lora_airtime}.h` |
| Risk | Events/messages/replay consume RX opportunity, starve responses and multiply relay airtime. |
| Target state | One bounded scheduler admission seam with deadlines, airtime fairness, priority reservations and verified regional policy. |
| Smallest change | Define send descriptor/admission ownership; add only needed live/event arbitration when competing M6 traffic exists. |
| Dependencies | Actual traffic budgets, G12/G14; jurisdiction and antenna verification for policy values. |
| Compatibility impact | R2 single-owner/gate and nonpreemptive TX preserved; no current scheduling/RF change. |
| Wire impact | None for local scheduling; future authenticated priority/hop fields in envelope. |
| Storage impact | RAM descriptors initially; durable retry state owned by corresponding service. |
| Tests | Starvation, response reservation, expired live data, retries, queue bounds, per-source fairness, relay amplification, conservative reboot budget. |
| Recommended milestone | Pre-M6 required boundary before competing critical traffic; full scheduler before messaging/backlog scale. |

### G18 — Security and replay lifecycle missing (P1; N)

| Field | Assessment |
| --- | --- |
| Current state | PHY CRC/private sync word only; uint32 packet dedupe is RAM; no authenticated issuer/keys. |
| Evidence | `protocol/M1_TEST_PACKET.md`, `firmware/include/packet_dedupe.h`, `firmware/src/network_service.cpp`, `firmware/include/radio_config.h` |
| Risk | Eavesdropping, spoofing, replay and forged contact; unacceptable future private location/config/control trust. |
| Target state | Reviewed library AEAD, per-device credentials, independent endpoint privacy, durable nonce/replay generation, authorization and ownership transfer. |
| Smallest change | Threat model now; dedicated nRF52840 library/resource/provisioning evaluation before protocol security implementation. |
| Dependencies | G05/G07/G14; entropy/physical key storage and reset/update policy. |
| Compatibility impact | Legacy input remains explicitly untrusted; no security downgrade or gateway trust laundering. |
| Wire impact | Future security envelope, likely v2; no primitives/tag lengths finalized. |
| Storage impact | Separate keys/counters/key generation; never reuse cyclic history as sole security nonce authority. |
| Tests | Published crypto vectors, tamper/replay, nonce crash/rollback, key rotation/transfer, compromised relay, unauthorized issuer and parser resource bounds. |
| Recommended milestone | Pre-security required; before authenticated M6 contact/receipts, remote config, private person tracking, messaging or control; before M7 exposure. |

### G19 — Hardware adaptation still concentrated in managers (P2; Q)

| Field | Assessment |
| --- | --- |
| Current state | Arduino/FreeRTOS/SX1262/RAK setup and Serial logging share RadioManager; GNSS manager owns u-blox/Wire; watchdog/time are nRF/RTOS-specific. |
| Evidence | `firmware/src/{radio_manager,gnss_manager,watchdog_manager,monotonic_time,i2c_recovery}.cpp`, `firmware/include/power_manager.h` |
| Risk | A second board requires changing application policies unless boundaries are kept small and explicit. |
| Target state | Board contracts for radio, sensors, clock, power, reset and diagnostics with stable core values. |
| Smallest change | G03/G04/G05 first; leave register/driver code inside current adapters until actual second hardware motivates extraction. |
| Dependencies | Actual second board; board contract tests and RAK physical baseline. |
| Compatibility impact | Same RAK configuration/patch guards; no Zephyr/new HAL/platform support now. |
| Wire impact | None; platform is not protocol semantics. |
| Storage impact | None unless new physical backend/layout is explicitly introduced. |
| Tests | RAK regression suite plus identical contracts on second board, bounded timings and callback ownership. |
| Recommended milestone | Before second hardware platform. |

### G20 — GNSS power modes and shared sensor resources need evidence (P1)

| Field | Assessment |
| --- | --- |
| Current state | M3 rail release/continuous short interval; role does not power-gate GNSS. R4 has GNSS/auxiliary owner bits; I2C recovery is driven by GNSS. |
| Evidence | `firmware/include/gnss_config.h`, `firmware/src/{gnss_manager,sensor_power_manager,i2c_recovery}.cpp`, `docs/audits/R4_I2C_WATCHDOG_POWER_FIX.md` |
| Risk | M6 slot/interrupt conflicts, shared bus resets or wrong rail assumptions; unmeasured backup policy harms reliability/energy. |
| Target state | Verified board resource map, per-consumer ownership, explicit supported GNSS modes and coordinated recovery. |
| Smallest change | Verify RAK1904 slot/WB_IO2 wiring; add its actual ownership/recovery needs without changing proven GNSS policy by default. |
| Dependencies | Physical assembly and power/TTFF measurements; G01 independent power policy. |
| Compatibility impact | Preserve idempotent bit ownership; no automatic full cycle after every fix; no role-implied GNSS off. |
| Wire impact | None. |
| Storage impact | None initially; future config stores explicit policy, not live rail state. |
| Tests | Two consumer leases, release ordering, I2C recovery with both sensors, watchdog feeding, current/backup/TTFF on hardware. |
| Recommended milestone | Pre-M6 required physical resource review; power optimization after measurements. |

### G21 — History retention and delivery are bounded foundations (P2)

| Field | Assessment |
| --- | --- |
| Current state | Maximum 728 POSITION records; no automatic RF backlog or receipt caller. Delivery/replay state APIs exist. |
| Evidence | `docs/storage/M4_FLASH_JOURNAL.md`, `firmware/include/history_store.h`, `firmware/src/position_flow.cpp` |
| Risk | “Store-forward” mistaken for working delivery; one FIFO for future traffic delays live data; two-week retention overstated. |
| Target state | Per-class retention/custody/receipt, live-first scheduling, eventual bounded backlog with original observation time. |
| Smallest change | Preserve journal; define authenticated receipt and history/live provenance before enabling replay. |
| Dependencies | G07/G14/G17/G18, server/BASE receipt contract. |
| Compatibility impact | TX_DONE remains nondelivery; no historical packet displayed as fresh due to arrival time. |
| Wire impact | Future ACK/history indication or envelope; unchanged POSITION may be nested with historical context. |
| Storage impact | Existing seven pages retained; new delivery semantics cannot retire undelivered gaps via an unsafe cumulative cursor. |
| Tests | Lost/out-of-order receipts, holes in sequence/record delivery, replay reboot, live preemption, retention overflow and no continuous resend after confirmation. |
| Recommended milestone | Future only, before backlog replay and expanded record families. |

### G22 — Backend/UI/gateway semantics not implemented (P3)

| Field | Assessment |
| --- | --- |
| Current state | BASE logs reception; no Android/IP/server bridge or device/user data model. |
| Evidence | `firmware/src/radio_manager.cpp:handleReceivedPacket`, `docs/milestones/M5.md`, `AGENTS.md` M8 |
| Risk | Arrival treated as measurement freshness; gateways become server-dependent; uint64 identity loses precision; UI assumes all nodes are trackers. |
| Target state | Stable device/user assignment model, source/age/trust and requested/reported distinctions, independent local bridge/relay. |
| Smallest change | Document canonical entities and UI semantics now, implement only after firmware gates. |
| Dependencies | G01/G05/G10/G14/G18 and actual transport integrations. |
| Compatibility impact | Preserve BASE direct/relay path observations and dedupe meaning; no invented live/delivery status. |
| Wire impact | None for conceptual model; future bridge metadata preserves original protocol/trust. |
| Storage impact | Future offline DB/server schemas, bounded bridge queues and revision/tombstone sync. |
| Tests | Offline reconnect, reordered sync, duplicate paths, lossless uint64 IDs, stale location/tank/valve values and server outage. |
| Recommended milestone | Future only, M8. |

### G23 — Test/version/DFU contracts need incremental expansion (P1)

| Field | Assessment |
| --- | --- |
| Current state | Strong focused host fault tests and dependency guards; no general fuzz, phone/config/QoS tests or proven DFU preservation/rollback. |
| Evidence | `firmware/tests/run_host_tests.sh`, `firmware/tests/{m3,m4,m5,r2,r3,r4,startup}/`, `firmware/rakwireless/boards/rak4630.json` |
| Risk | Boundary moves regress hardening; future schema downgrade erases keys/config or reuses security state. |
| Target state | Independent versions, exact legacy vectors, fault/migration/board contracts and explicit update footprint. |
| Smallest change | Add missing independent legacy vectors when extracting boundaries; implement future tests alongside actual features, not speculative frameworks. |
| Dependencies | G07/G18 for update/security; physical bootloader evidence. |
| Compatibility impact | Firmware version cannot reinterpret protocol or storage version; downgrade unsupported state preserved. |
| Wire impact | None for regression work. |
| Storage impact | None now; later migration/DFU tests verify preservation and anti-rollback requirements. |
| Tests | Host ASan/UBSan, exact codec fixtures, malformed/fuzz, cut-point migrations, RAK board chain and later DFU interruptions. |
| Recommended milestone | Pre-M6 required legacy regression boundary; pre-M7 required update/storage plan. |

### G24 — Physical validation incomplete (P1)

| Field | Assessment |
| --- | --- |
| Current state | Owner reports two-node RF and GNSS detection/start/timeout; full GNSS POSITION chain and injected recovery/power behavior remain unproven. |
| Evidence | Owner's audit request physical evidence list; `docs/audits/R4_I2C_WATCHDOG_POWER_FIX.md`, `docs/storage/M4_FLASH_JOURNAL.md`, M2–M5 reports |
| Risk | New integration hides unresolved electrical/system failures; compile success mistaken for field readiness. |
| Target state | Recorded board/firmware/slot/antenna/test procedure and measured outcomes at each physical gate. |
| Smallest change | Open-sky GNSS→store→POSITION→BASE first, then real three-node relay and M6 slot/rail verification. |
| Dependencies | Physical devices, outdoor conditions and fault/power measurement equipment. |
| Compatibility impact | Test existing production behavior; restore production after temporary instrumentation. |
| Wire impact | None. |
| Storage impact | No design change; power-cut tests may sacrifice designated test history only in a separately authorized hardware session. |
| Tests | Pending full chain, stuck SDA/SCL, deliberate watchdog stall, flash cuts, electrical rail, current, field range and enclosure. |
| Recommended milestone | Immediately required evidence tracking; relevant hardware gates before declaring M6 integration/field readiness. |

### G25 — Battery / energy observability and power-health policy (P2; new 2026-09-23 finding)

| Field | Assessment |
| --- | --- |
| Current state | `battery_capacity_mah` is a durable ConfigStore field but is informational only. Production has no authoritative battery voltage/SOC/charge-state/solar observation service, no low/critical battery policy, no user-visible energy-health contract and no measured end-to-end runtime budget. M7P7B quantitative BLE current remains explicitly deferred. |
| Risk | A field tracker can become energy-degraded or fail without the product distinguishing low battery, charging failure, abnormal consumption, radio/GNSS availability tradeoffs or an actually dead/offline device. Capacity configured in mAh can also be mistaken for measured state if the UI boundary is not explicit. |
| Target state | A bounded power-health owner separates configured capacity from measured voltage/SOC/charge/external-power/solar observations, exposes validity/age/health, and feeds explicit low/critical power policy without silently rewriting requested services. Runtime/endurance claims are based on hardware measurements under representative GNSS/RF/BLE/storage workloads. |
| Smallest change | First verify the actual owned RAK battery/charge measurement path and available signals; add only the minimal normalized observation/diagnostic seam required by that hardware. Do not invent coulomb counting or solar telemetry unsupported by the board. Policy thresholds and adaptive behavior come only after measurement evidence. |
| Dependencies | Current power/resource ownership; Health/Diagnostics direction; representative enclosure/battery/solar hardware; G17 if energy policy changes RF/backlog admission. Protected remote power-policy changes additionally depend on G18. |
| Compatibility impact | No TLP v1 reinterpretation. Existing tracking/RF/GNSS behavior remains unchanged until an explicit reviewed power-policy milestone authorizes degradation or scheduling changes. |
| Wire impact | None for local observation first. Any later battery telemetry/event bytes require an explicitly versioned telemetry/event contract rather than overloading POSITION. |
| Storage impact | No new persistence is required for the first live health observation. Durable energy history/threshold configuration requires an explicit schema/retention owner and must not be mixed into HistoryStore by convenience. |
| Tests | Sensor/ADC validity and scaling, absent/unsupported measurement path, stale/unknown state, threshold hysteresis if later introduced, charging/external-power transitions, reset behavior, and physical current/runtime measurements across representative tracker states. |
| Recommended milestone | Record now; implement/measure before a field pilot or any product claim about battery percentage, remaining runtime, low-battery alarm reliability or solar autonomy. It does not block M7P7G or the secure TLP v2 foundation. |

## Priority roll-up

Historical 2026-09-14 roll-up: P0: none confirmed. P1: G01–G05, G07–G10,
G12, G14, G17–G18, G20, G23–G24. P2: G06, G11, G13, G15, G19, G21.
P3: G16, G22. G25 was identified by the 2026-09-23 coverage review and is P2,
with a before-field-pilot timing gate. These are timing gates, not instructions
to implement every P1/P2 immediately; use the coverage table above for current
closure state.

Expected minimum areas A–Q are explicitly mapped in finding headings: A/G01,
B/G02, C/G03, D/G04, E/G05, F/G06, G/G14, H/G08, I/G09, J/G10,
K/G11, L/G12, M/G13, N/G15+G18, O/G16, P/G17, Q/G19.

## SMALLEST SAFE PRE-M6 CHANGE SET

1. **Establish evidence and freeze compatibility.** Preserve the pinned main
   baseline, independent TEST/POSITION/RELAY vectors and startup-ID regression.
   Run the existing host suite and PlatformIO for subsequent firmware work.
   Complete the open-sky chain and verify RAK1904 slot/rail/interrupt mapping
   before claiming hardware integration readiness (G23/G24/G20).
2. **Extract only portable values and identity.** Neutral location/GNSS value
   header, exact legacy POSITION mapping, small identity boundary available
   before radio failure. Leave GNSS recovery, radio gate and journal untouched
   (G03–G05).
3. **Separate configuration meaning with compatibility mapping.** Introduce
   role/profile/capability/source/power values and whole-candidate validation;
   route existing USB commands through a shared handler. Keep current AUTO,
   defaults and effective behavior for unprovisioned legacy operation; saved
   explicit intent takes precedence when persistence arrives (G01/G02/G08/G09).
4. **Approve persistence ownership before writing new durable categories.**
   Prove bootloader/SoftDevice/DFU boundaries, growth and capacity. A config-store
   implementation is a separate layout-tested change, not a prerequisite for
   an isolated accelerometer read. It is mandatory before persistent settings,
   phone state, durable new events or security metadata (G07).
5. **Use a GNSS-only source-neutral location handoff for M6 consumers.** Preserve
   freshness/store-first semantics. Do not implement PHONE, NODE_LOCATION or
   AUTO source switching merely to satisfy the interface (G10).
6. **Specify critical-event/contact and send admission before adding competing
   M6 radio traffic.** Local activity/geofence may proceed; trusted network
   contact/LOST and event receipt require the security/envelope gates. Full
   generic forwarding, messaging scheduler and mailbox are deferred (G12/G14/G17/G18).

Each item should be a small reviewable task with its own behavior/byte/storage
invariants and tests. No new HAL, telemetry format, messaging, actuation, Android,
backend, BLE startup or protocol migration belongs in this pre-M6 boundary-only
set. R1–R4 are retained, not rewritten for architectural purity.
