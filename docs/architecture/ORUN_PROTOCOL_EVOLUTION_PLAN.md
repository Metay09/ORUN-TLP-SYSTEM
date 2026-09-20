# ORUN Protocol Evolution Plan

Date: 2026-09-14. Baseline: `aa3bbf810a37034d9a3d9066ede4bf579646adfe`.
Status: architecture recommendation, not a new wire specification.
All existing protocol/firmware files remain unchanged in this milestone.
See [system architecture](ORUN_SYSTEM_ARCHITECTURE_V1.md) for ownership, service,
security and QoS contracts and [gap register](ORUN_ARCHITECTURE_GAP_ANALYSIS.md)
for timing and required tests.

## 1. Audited v1 contract

| Family | Version/type | Bytes | Current semantics |
| --- | --- | ---: | --- |
| TEST | 1 / 0x01 | 18 | Source uint64, sequence uint32, uptime uint32; production periodic beacon disabled |
| POSITION | 1 / 0x02 | 34 | Source uint64, sequence uint32, GNSS UTC, E7 coordinates, ellipsoid altitude mm, HDOP ×100, satellites and validity flags |
| RELAY_FORWARD | 1 / 0x03 | 49 | Relay ID, hop one, original length 34, ingress RSSI/SNR, exact unchanged inner POSITION |

Canonical layouts: [TEST](../../protocol/M1_TEST_PACKET.md),
[POSITION](../../protocol/M2_POSITION_PACKET.md),
[RELAY_FORWARD](../../protocol/M5_RELAY_FORWARD_PACKET.md).
Codecs: `firmware/src/tlp_test_packet.cpp`, `tlp_position_packet.cpp`,
`tlp_relay_forward_packet.cpp`. Multi-byte fields use explicit big-endian encoding;
C++ struct layout is neither wire nor persistent layout.

Strict decoders require exact lengths/version/type. POSITION rejects reserved
flag bits and out-of-range coordinates. Its codec does not require valid_fix
to be set or enforce all semantic relationships between flags/time. Future
location/geofence consumers must validate those semantics before using received
data; successful decoding alone is not evidence of a trustworthy fresh fix.
Its UTC-valid flag disambiguates time; no location source, accuracy-metres,
revision, age or historical/live marker is available. Never repurpose satellite
count, HDOP, reserved bits, source identity or timestamp as another field.

`RadioManager::handleReceivedPacket` dispatches supported types; unsupported
packets are rejected, not generically forwarded. NetworkService validates v1
and only handles POSITION/RELAY_FORWARD. A RELAY rejects relay envelopes and
never forwards them; TRACKER never relays. BASE dedupes application POSITION
by original `(source_device_id, sequence_number)`, retaining separate direct and
relay path observations. Relay queue is four entries; caches are 16/32 entries.
These bounds and hop policy are unchanged here.

R1 persistent reservations reduce sequence reuse across supported resets but
uint32 wire sequence still wraps. RAM dedupe evicts and resets. Neither provides
cryptographic anti-replay. CRC detects transmission errors; the private sync word
is public configuration. No authentication/encryption, destination, ACK, command
result, human-message delivery or automatic backlog replay exists in v1.

## 2. Evaluate the three migration choices

| Choice | What works | Limits/cost | Verdict |
| --- | --- | --- | --- |
| A: Add distinct v1 types | Independent strict codecs can coexist on upgraded receivers; existing 0x01–0x03 remain byte-identical | Legacy nodes ignore new types; M5 relay cannot carry them; each family may duplicate routing/security headers | Technically safe for a justified direct-only experiment with explicit compatibility tests; not a complete network evolution |
| B: Future v2 network envelope | Shared addressing, stable identity, priority, security and bounded generic forwarding around distinct application families | More airtime/RAM, two decoders during transition, careful nonce/hop design and upgrade ordering | Preferred before secure commands and generic new-family forwarding |
| C: New v1 wrapper containing generic/security envelope | Could leave old packets intact while introducing a new outer type | Same legacy relay incompatibility as v2; “v1” would conceal two security/framing models without saving material complexity | No demonstrated benefit over explicit v2; reconsider only with measured interoperability evidence |

Continue v1 for the current RAK product and validation. **Prefer a future v2
network envelope** for secure multi-service operation. New version does not mean
LoRaWAN or a different RF profile. This document does not implement v2, fix its
layout, select crypto or assign any additional packet IDs.

If M6 needs local events before v2/security, they can be represented locally;
do not pretend unauthenticated TX completion proves remote contact. A provisional
new v1 EVENT type would need its own review and clear lack-of-security status,
and would not traverse unmodified M5 relays. Avoid building multiple temporary
formats unless an actual delivery need justifies them.

## 3. Future application semantics and allocation discipline

| Family | Meaning and required semantic boundary | Version/transport recommendation |
| --- | --- | --- |
| POSITION | Moving, time-sensitive observation; preserve original measurement freshness/provenance | Keep legacy v1 codec; future source-neutral schema separate from legacy GNSS bytes |
| NODE_LOCATION | Configured/known physical position, source, revision, time quality, validity, optional accuracy/altitude, movement suspicion | New family; usually v2, a direct-only v1 variant only with separate justification |
| EVENT | Stable occurrence ID, type/severity, onset/update/clear, time and context | v2 for authenticated receipt/critical behavior |
| TELEMETRY | Metric-instance IDs, scaled integers, quality, observation time/age, bounded batch | New schema after encoding comparison; v2 transport preferred |
| COMMAND | Issuer/destination, ID, typed arguments, expiry and authorization | v2 security prerequisite; never unauthenticated legacy fallback |
| COMMAND_RESULT | Accepted/executed/failed/unknown with reasons and feedback linkage | Correlated to command; not ordinary telemetry or TX_DONE |
| NODE_INFO | Device/hardware/firmware, supported versions/capabilities/services, effective config revision | Rate-limited discovery; identity claims require authentication for trust |
| HEALTH | Aggregated reset/GNSS/radio/flash/sensor/queue/security diagnostics | Optional compact family or defined telemetry schema, decide by payload analysis |
| MESSAGE | User/device sender, recipient, stable message ID, short text/status/emergency, TTL and endpoint privacy | v2 addressing/QoS and optional bounded fragmentation |
| ACK | Explicit receipt scope and original identity, status/received range where safe | Authenticated for trusted receipt; never implies execution/read |

Do not allocate IDs until a registry proposal states version, owner, schema,
maximum encoded length, unknown-field behavior, security/delivery expectations,
golden vectors and legacy-path behavior. Never reuse retired IDs within a version.
Keep protocol version separate from payload schema, config/storage formats and
key generation. NODE_INFO must not be required before every useful packet;
capability negotiation must be cached/rate-limited and must not enable security
downgrade based on an unauthenticated advertisement.

## 4. Generic envelope: minimum responsibilities, not final fields

The envelope needs enough structure to reject malformed input, identify origin
and intended audience, suppress duplicates, enforce forwarding/admission and
bind security context. A candidate semantic decomposition is:

| Element | Recommendation and tradeoff |
| --- | --- |
| Framing version and bounded length | Required; explicit total/header/payload boundaries or a provably unambiguous equivalent; no unbounded optional-field scan |
| Application family/schema | Required for destination dispatch; relay may carry allowlisted unknown optional families without understanding content |
| Origin identity | Stable DeviceIdentity reference; compression only with authenticated collision-safe context, no implicit hardware model |
| Destination scope/address | Needed for commands/messages; distinguish node, local broadcast and future group; no broadcast actuation default |
| Stable network message identity | Origin + durable generation/counter candidate; evaluate whether this also covers packet sequence so redundant IDs can be omitted |
| Fragment identity | Only if fragmentation enabled; stable parent ID plus bounded index/count/total size |
| Priority/flags | Small authenticated class with local authorization/rate caps; reject unknown critical flags |
| Hop allowance | Original authorized maximum one RF relay; mutable consumed-hop metadata must not let peers expand it |
| Expiry/time context | Required for expiring messages/commands; absolute UTC with quality or defined authenticated relative/session mechanism |
| Security context | Key generation/context, nonce construction inputs and authentication tag; lengths/library remain undecided |
| Link/path observations | Optional relay-local metadata; do not burden every direct application frame with RSSI/SNR fields |

Avoid carrying both sequence and message ID unless distinct replay, fragment or
application requirements justify them. App event/command/message IDs may persist
across multiple transport attempts; network counters used for nonce uniqueness
have a different lifecycle. Never derive a nonce solely from current wrapping
v1 sequence or reused boot timestamp.

Current driver receive ceiling is 255 bytes, with a four-entry copied event
queue in `radio_manager.cpp`; this is a hard local bound, **not** a recommended
application payload size. Security/header/tag overhead reduces payload space.
Choose a smaller practical MTU from measured airtime, RAM and retry costs. Long
SF11 packets block RX and cannot be preempted for a later critical event.

### Immutable content versus forwarding metadata

Authenticate origin, destination, family, message identity, authorized priority,
original hop limit, expiry and payload boundary with the endpoint payload.
Keep routing-visible metadata minimal. A relay cannot alter an authenticated
hop counter covered by an end-to-end tag without invalidating it.

Evaluate either an immutable protected inner message plus a separately
authenticated bounded relay wrapper, or an immutable end-to-end region with a
specified hop-authenticated outer region. The former is simpler to reason about
for a single hop but adds overhead; the latter can be smaller but makes mutation
rules more complex. Neither is finalized here. A mutable unprotected byte is
insufficient enforcement against malicious hop resetting. Even authenticated
malicious relays can drop/delay traffic; the threat model must state this limit.

Do not forward arbitrary noise or unknown framing versions. Validate membership
where required, version/length, allowed routing scope, expiry, hop eligibility,
dedupe and available airtime before queueing opaque content. Destination rejects
unsupported required schema/features; optional bounded data may be skipped as
specified. Avoid error replies to unauthenticated malformed frames (reflection).

## 5. Coexistence and deployment sequence

| Sender/path | Legacy receiver/relay | Upgraded receiver/relay |
| --- | --- | --- |
| v1 direct POSITION | Current behavior | Dedicated legacy decoder; label unauthenticated |
| v1 RELAY_FORWARD | Current BASE decodes; RELAY rejects | Dedicated legacy decoder, original identity/path preserved |
| Additional v1 family | Unknown type rejected; no M5 forwarding | Explicit new codec only if implemented and provisioned |
| v2 frame | Unsupported version/type, no effects/forwarding | v2 parser/security/admission and supported app dispatch |
| v2 private/control through legacy-only relay | No supported path | Requires upgrade/replacement of relay path, not automatic plaintext fallback |

1. Freeze legacy golden vectors and malformed behavior; keep current firmware
   field-testable. Record node identity and supported firmware on actual devices.
2. Approve v2 envelope/security/airtime/persistence specification and independent
   test vectors, including the one-hop/downlink reachability policy.
3. Upgrade designated gateways and relays to dual parsing while retaining exact
   v1 handling. Keep new families disabled until their entire path is verified.
4. Provision/authenticate endpoints and enable v2 for a selected cohort; test
   mixed direct/relay, reboot, loss and saturation. Preserve DeviceIdentity.
5. Retain legacy v1 receive support for the agreed migration period; mark all
   legacy observations untrusted. Enable v1 TX only by explicit legacy policy.
6. Retire legacy transmit support only after owner decision and fleet evidence;
   no main merge or deployment occurs in this milestone.

Do not broadcast every datum in both versions by default: it doubles load and
complicates dedupe. If a temporary dual publication is necessary, define a shared
observation identity at the upgraded receiver, scope/time-limit it and budget
both emissions. An IP gateway must not stamp its receipt time over old GNSS time
or claim authenticated device origin for an imported plaintext v1 packet.

Gateway-to-gateway bridging needs origin/domain-aware duplicate suppression and
end-to-end TTL. The IP segment is not permission to reintroduce a packet
indefinitely into LoRa. RF hop allowance remains one within each explicitly
modeled leg; the bridge route policy must bound the full path and prevent return
to an already visited ingress domain. No general mesh/routing protocol is chosen.

## 6. ACK, retries, history and QoS

ACK scope is explicit: network acceptance, durable custody, destination node
receipt or destination application receipt. Command RESULT and message READ
are separate app evidence. TX_DONE is never any of these. Receipt correlation
includes full original identity/generation and appropriate issuer authentication.
Normal telemetry need not receive an ACK per packet. Critical events/config and
important results use bounded policy; retry same app ID, not a new occurrence.

Do not use a cumulative “delivered through sequence N” receipt to erase gaps that
were never received. Existing history cursor APIs need a proven contiguous-record
or explicit selective-ack interpretation, considering skipped sequence reservations
and shared TEST allocations. Old POSITION bytes may be preserved inside a future
envelope carrying historical context, but do not enable replay to a UI that uses
arrival as evidence of live position.

QoS remains local admission plus authenticated hints: bounded queue per class,
reserved response capacity, deadline expiry, airtime fairness, rate limits,
backoff and relay amplification accounting. Critical flags are authorized;
ordinary chat cannot promote itself to emergency without limit. Unknown optional
traffic is lower-trust admission work, not automatically forwarded safety traffic.

## 7. Security envelope options and specification gate

Network AEAD can protect node traffic and routing claims while an independent
endpoint E2E layer protects human text from relays/gateways. Shared network/group
keys simplify forwarding but enlarge compromise scope; per-device credentials
improve containment and provisioning complexity. Compare these against actual
offline topology, endpoint key distribution and RAK resources before selection.

No cipher, key size, nonce layout, tag truncation or library is finalized here.
Use mature reviewed libraries after actual nRF52840/Arduino support, entropy,
RAM/flash, published vectors, key storage and maintenance review. Do not invent
crypto or import LoRaWAN packet/join semantics. Security counter reservation,
replay windows, key rotation, reboot/rollback and factory reset are prerequisites,
not optional follow-up work after COMMAND packets ship.

End-to-end ciphertext forwarding is a design requirement for future private
messaging. It does not conceal traffic metadata or prevent malicious forwarding
nodes from denying service. Protect bounded fragment handling and authentication
ordering against memory/CPU exhaustion; preferably ship short unfragmented
messages first.

### M7P6D pre-wire security contract

The focused M7P6D design record now narrows the security choices before a v2
layout is allocated:

- directions are cryptographic endpoint directions (`D2A`, `A2D`), not
  TRACKER/RELAY/BASE roles;
- D2A and A2D traffic keys are independently derived with HKDF-SHA256;
- the candidate AES-CCM construction uses a 13-byte nonce consisting of
  key epoch, direction and the sender's durable 64-bit security counter;
- gateway/relay forwarding does not own keys, counters or replay acceptance;
- replay state changes only after successful AEAD authentication;
- device A2D starts with strict durable monotonic admission, while the backend
  may use a bounded D2A sliding window for legitimate multi-path reordering;
- the exact current Bluefruit/CC310 coexistence behavior must be physically
  proven before production secure-envelope code uses CC310 in the normal
  packet path.

These rules do not allocate any wire field. The eventual v2 header must expose
or unambiguously supply the authenticated context necessary to reconstruct the
selected key and nonce. See `docs/milestones/M7P6D.md`.

## 8. Validation and release checklist

Before any wire implementation, supply independent byte vectors for every new
family and every supported legacy packet; verify signed boundaries, exact lengths,
optional fields and critical unknowns. Run malformed/fuzz input under sanitizers,
security published vectors, replay/nonce crash tests, bounded queue/fragment tests,
and mixed-version network simulation. Run all existing R1–R4/startup/serial tests
and PlatformIO after production changes. Finally measure real on-air frames,
airtime, multi-device relay, loss/retry and current on RAK hardware.

Owner decisions: v2 header budget, identity namespace, supported application
schema/ID allocation, authenticated contact/ACK meaning, relay-visible security
context, provisioning/key library, packet MTU, downlink rendezvous, regional
policy and legacy support duration. Their absence blocks v2 implementation, not
this documentation review.

**Milestone invariants:** TEST remains 18 bytes; POSITION remains 34 bytes;
RELAY_FORWARD remains 49 bytes with exact original POSITION and one-hop rule;
RF configuration, firmware behavior and history v3 are unchanged. No IDs were
allocated and no production protocol code was modified.
