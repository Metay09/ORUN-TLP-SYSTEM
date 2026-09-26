# ORUN TLP v2 delegated command security contract — DRAFT

Status: **DESIGN CANDIDATE — NOT OWNER-APPROVED, NOT WIRE-FROZEN, NO PRODUCTION RUNTIME AUTHORIZED.**

Baseline: `main@e2a370510c595c5f4b88e94a1212fb95d848a273`.

Branch: `design/tlp-v2-command-security-contract`.

This document is the focused follow-up required by
`ORUN_GATEWAY_COMMAND_AUTHORITY_DIRECTION.md` after M7P6F. It resolves the
first concrete candidate for delegated gateway command authority, replay,
revocation, command freshness/idempotency and a bounded TLP v2 command envelope
without modifying firmware, TLP v1, SecurityStore, RF behavior or provisioning.

It is deliberately a review artifact. Independent security/protocol review and
owner approval are required before any numeric field, KDF label, storage schema
or runtime behavior becomes implementation-frozen.

## 1. Product objective

The command path must support all of these at once:

```text
authorized user
    |
    | internet may disappear after authorization/grant refresh
    v
enrolled gateway
    |
    | creates final self-contained protected command
    v
opaque relay
    |
    | may persist the unchanged ciphertext for minutes/hours
    v
sleepy tracker RX opportunity
    |
    | authenticate -> replay check -> application precondition -> apply
    v
authenticated RESULT
```

The user may leave after creating the command. The originating gateway does not
need to remain RF-adjacent to the tracker after an opaque relay has custody.

The tracker must never need to know whether Internet exists.

## 2. Preserved invariants

This candidate preserves:

- `Role != Location Source != GNSS Power != Capability != Transport != Identity != Profile != User Identity != Security Authority`;
- backend remains canonical owner of user authorization and gateway enrollment;
- tracker stores no human/user/phone ACL;
- normal relay remains opaque and receives no tracker root key;
- no fleet-wide/group authentication key;
- gateway enrollment does not make a gateway the tracker configuration owner;
- TLP v1 TEST/POSITION/RELAY_FORWARD bytes remain unchanged;
- no protected operation may silently fall back to unauthenticated TLP v1;
- replay protection, freshness, idempotency, delivery and RESULT remain distinct;
- unauthenticated input may not mutate durable security state.

## 3. Security contexts

Three security paths remain distinct.

### 3.1 Backend A2D

The existing M7P6D/M7P6F backend/asynchronous A2D path remains valid.

It continues to use the already-reviewed device credential lifetime and M7P6F
durable A2D replay state. This draft does not reinterpret that state as gateway
state.

### 3.2 Device D2A

The existing M7P6D device-to-authority direction remains the normal secure
uplink authority path. The device-owned durable TX counter remains independent
from TLP v1 sequence/history counters.

### 3.3 Delegated gateway context

An enrolled gateway may receive narrowly scoped, **per-tracker** delegated key
material from the backend authority service.

The gateway does not receive `K_root`.

For one tracker credential lifetime, a delegated context is identified by:

```text
tracker credential_id
tracker key_epoch
gateway_device_id
gateway_policy_epoch
scope_id
quota_code
```

`gateway_device_id` is public identity. Enrollment authority is represented
by possession of the derived delegated key for the current policy epoch, not by
the public ID alone.

A gateway factory reset destroys its delegated keys and sender state. Re-
enrollment produces a newer gateway policy epoch before that gateway can again
originate trusted commands.

## 4. Gateway policy epoch: revocation and safe replay-slot reclamation

Each tracker credential lifetime has one logical `gateway_policy_epoch`,
owned by the backend authority.

Adding, removing, factory-resetting or materially changing the command
authorization of a gateway advances this epoch.

When the epoch advances:

1. the backend issues new delegated material only to gateways that remain
   authorized;
2. no tracker-by-tracker USB/service action is required;
3. the tracker learns the newer epoch from the first **successfully
   authenticated** delegated frame at that epoch;
4. the tracker durably commits the newer epoch before dispatching that frame;
5. all delegated replay contexts from lower epochs become permanently stale.

A lower epoch is always rejected once a higher epoch is durable.

This allows old replay slots to be reclaimed safely after an epoch advance:
old ciphertext cannot become valid again merely because its per-gateway HWM was
discarded.

A random or unauthenticated frame carrying a higher epoch cannot advance the
floor; authentication under a correctly derived newer-epoch delegated key is
required first.

### Residual revocation window

A completely offline tracker cannot learn a central revocation immediately.

Therefore an old gateway epoch remains usable until either:

- the tracker accepts a valid newer-epoch delegated frame; or
- the old delegated grant exhausts its cryptographically bound command quota.

This is an explicit offline availability/security tradeoff, not hidden expiry.

## 5. Cryptographically bounded gateway quota

Delegated gateway authorization uses a bounded command budget without requiring
a trusted tracker wall clock.

The candidate two-bit `quota_code` mapping is:

| code | maximum sender counter in one gateway policy epoch |
| ---: | ---: |
| 0 | 15 |
| 1 | 63 |
| 2 | 255 |
| 3 | 1023 |

Counters start at 1.

The backend chooses the quota appropriate to the enrolled gateway and command
scope. A gateway cannot enlarge its own quota because `quota_code` is part of
the delegated-key derivation and the authenticated header.

The system-wide stale-user/stale-gateway risk is therefore finite even if no
trusted time progresses. A higher-risk future actuation scope may choose a
smaller quota than ordinary configuration management.

## 6. Delegated key derivation candidate

The existing credential PRK remains:

```text
PRK = HKDF-Extract(
        SHA-256,
        salt = credential_id[16],
        IKM  = K_root[32])
```

For delegated gateway traffic:

```text
info =
    ASCII("ORUN-TLP-V2-GW")        // 14 bytes, no NUL
    || delegated_direction_u8      // GW2D=0x03, D2GW=0x04
    || key_epoch_be32
    || gateway_device_id_be64
    || gateway_policy_epoch_be32
    || scope_id_u8
    || quota_code_u8

K_delegated = HKDF-Expand(SHA-256, PRK, info, 16 bytes)
```

The backend/HSM/authority service derives the exact per-tracker key and delivers
it to the enrolled gateway over the later reviewed enrollment channel.

The tracker derives the same key from its own `K_root` and the authenticated
header candidate.

Different `scope_id` values produce different keys. A gateway that possesses a
configuration key therefore cannot retag a packet as a future actuation scope
without the corresponding independently issued key.

All scope keys for one
`(tracker, gateway_device_id, gateway_policy_epoch, quota_code)` share the same
gateway sender counter. This keeps one tracker replay HWM per gateway rather
than one slot per command family.

The exact KDF above requires new ORUN-specific host vectors and a matching
RAK4631 KAT before implementation. M7P6E proves the primitive/coexistence path,
not these new delegated-label bytes.

## 7. Nonce and counter ownership

### 7.1 Gateway -> device

Candidate nonce:

```text
key_epoch_be32
|| 0x03
|| gateway_sender_counter_be64
```

The gateway exclusively owns this counter for one
`(tracker, gateway_device_id, gateway_policy_epoch)` sender context.

It is shared across all delegated scope keys at that epoch.

The gateway must reserve sender counters durably **before** returning one to the
packet builder. Candidate reserve-ahead block: 64 counters, clipped at the
grant quota ceiling.

A reboot burns any unused reserved counters. Counter rollback, reuse or wrap
fails closed.

### 7.2 Device -> delegated gateway RESULT

Candidate nonce:

```text
key_epoch_be32
|| 0x04
|| device_tx_counter_be64
```

The device may reuse its already durable, device-owned security TX counter
allocator because D2GW uses a distinct delegated traffic key and nonce direction
byte. No second device TX allocator is introduced.

The gateway keeps replay state for received tracker RESULT frames separately
from its outbound sender state.

## 8. Gateway secure-storage ownership

Delegated gateway material is not ConfigStore, HistoryStore, BLE bond storage or
tracker SecurityStore state.

A later implementation requires a dedicated gateway-authority persistence owner
that durably stores, per tracker as needed:

- tracker public identity / credential binding reference;
- current gateway policy epoch;
- authorized delegated scope key(s);
- quota code;
- TX reserve bound/runtime counter;
- received RESULT replay state as required.

Security material is never exported through normal diagnostics.

A raw backup/restore of an older gateway authority store is not a supported way
to resume enrollment. If rollback cannot be excluded, protected command
origination fails closed and the gateway re-enrolls.

Factory reset erases this authority store.

For 1000 trackers, per-target delegation is expected to consume tens of
kilobytes rather than RAM-only state. The future implementation must therefore
budget flash explicitly; it must not attempt to keep all grant material in RAM.

## 9. Tracker durable gateway replay state

The initial candidate bound is **4 simultaneously active command-capable
gateways per tracker policy epoch**.

The tracker persists:

```text
gateway_policy_epoch_floor
slot[4]:
    gateway_device_id
    gateway sender replay HWM / durable bound
    quota_code
    binding/check data required by the reviewed on-flash format
```

Rules:

- no silent LRU/age eviction;
- an existing gateway updates only its own slot;
- an unknown gateway at the current epoch is rejected if all four slots are in
  use;
- lower policy epochs are rejected;
- a successfully authenticated higher epoch is committed atomically before its
  first command dispatch, after which lower-epoch slots are safely reclaimable;
- ambiguous/corrupt recovery fails protected delegated reception closed;
- unauthenticated input never allocates or advances a slot.

This requires a separately reviewed persistence schema after this design is
approved. M7P6F SecurityStore v2 is not silently repurposed.

If the product later needs more than four simultaneously command-capable
gateways per tracker, increase the bound through an explicit schema/capacity
change rather than hidden eviction.

## 10. Offline user authorization proof

Human authorization remains separate from the gateway->tracker cryptographic
grant.

The candidate offline proof is a **backend-signed, gateway-independent
OfflineUserGrant** carried by the application and verified by an enrolled
gateway using a backend verification key anchored by the enrollment/
commissioning process.

Semantically it binds at least:

```text
grant_id
user/account subject
authorization generation
allowed operation scope
offline operation budget
backend signature
```

No tracker sees or stores this user grant.

The proof is portable: the same authorized user can present it to another
eligible enrolled gateway without moving user ACLs into tracker firmware.

Without a trusted progressing clock, immediate offline revocation is impossible.
Therefore each gateway durably consumes a bounded offline-operation budget for
the presented grant. Internet refresh obtains a new generation/budget.

Because a portable grant can be used at several already-enrolled gateways, the
worst-case stale-user authorization bound is the sum of the remaining per-
gateway offline budgets. This residual is explicit and should be visible in
backend policy.

The exact signature suite is **not** selected by this draft. It must be a mature
standard with a physically proven verify path on the chosen gateway platform;
the product must not invent a signature construction or derive tracker keys
from the user PIN.

## 11. Command freshness without tracker wall-clock time

A generic security TTL is not claimed when the tracker has no trustworthy
progressing clock.

Instead, first command families use **state preconditions**:

- configuration writes carry an expected configuration revision/generation;
- if current revision does not match, the tracker returns
  `STALE_PRECONDITION`;
- if the desired configuration already matches current durable state, the
  operation may return an idempotent `ALREADY_SATISFIED` result without another
  flash write;
- retries use the same application `command_id` but a fresh security counter.

For a future non-idempotent/high-risk actuation where executing a delayed command
would be unsafe, delayed store-forward is **not authorized** until that command
family defines an explicit target-issued freshness/challenge or equivalent
reviewed precondition.

Relay retention time is an operational queue policy, not cryptographic
freshness.

## 12. Reset-safe application idempotency

The security counter prevents reaccepting the same protected frame. It does not
make the logical command exactly-once.

The first protected mutation should therefore be a desired-state configuration
write using ConfigStore as the application owner.

The future application seam needs a read-only configuration generation/revision
accessor before remote CAS can be implemented; do not infer revision from
transport request IDs.

For initial configuration writes:

```text
authenticate
-> delegated replay durability
-> check expected config revision
-> validate complete candidate
-> ConfigStore requestSave()
-> durable save/readback complete
-> authenticated RESULT
```

A retry with the same `command_id` and desired value can safely return
`ALREADY_SATISFIED` if the durable state already equals the requested state.

Do not introduce a generic persistent command journal merely for hypothetical
future actuators. A later truly non-idempotent command family must define its
own durable PREPARED/APPLIED/RESULT or equivalent idempotency owner before it is
enabled.

## 13. TLP v2 secure application frame — candidate exact layout

This is the first exact wire candidate for review.

All multi-byte integers are big-endian.

Maximum **inner secure frame size: 80 bytes**.
Maximum ciphertext/application plaintext: **32 bytes**.
Authentication tag: **8 bytes**.

```text
offset size field
0      1    version = 0x02
1      1    type = 0x01                 // SECURE_APP candidate
2      1    header_len = 40
3      1    grant_flags
4      1    app_family
5      1    security_context
6      1    hop_limit
7      1    ciphertext_len              // 0..32
8      8    origin_device_id
16     8    target_device_id
24     4    key_epoch
28     8    security_counter
36     4    authority_generation
40     N    ciphertext                   // N = ciphertext_len
40+N   8    AES-CCM tag
```

Total size = `48 + N`, therefore 48..80 bytes.

Candidate `security_context` values:

```text
0x01 = BACKEND_A2D
0x02 = DEVICE_D2A
0x03 = DELEGATED_GW2D
0x04 = DELEGATED_D2GW
```

For delegated frames:

- `origin_device_id` is the gateway DeviceIdentity for GW2D and tracker
  DeviceIdentity for D2GW;
- `target_device_id` is the tracker for GW2D and gateway for D2GW;
- `authority_generation` is `gateway_policy_epoch`;
- `grant_flags[1:0]` is `quota_code`;
- `grant_flags[5:2]` is `scope_id`;
- `grant_flags[7:6]` must be zero.

For backend/device base contexts, reserved delegated grant bits must be zero.

The **AAD is exactly bytes 0..39**.

The receiver rejects before crypto if:

- version/type/header length is wrong;
- reserved bits are nonzero;
- ciphertext length exceeds 32 or does not match total frame length;
- target identity does not match the local intended destination;
- hop limit is not supported by the active path;
- security context is unknown.

Identity and routing fields remain untrusted until AEAD succeeds, but malformed
bounds are rejected before crypto to protect CPU/RAM.

### Candidate application families

```text
0x01 = COMMAND
0x02 = RESULT
```

These values are provisional until owner approval of this document.

## 14. COMMAND/RESULT plaintext candidate

### 14.1 COMMAND

Maximum plaintext remains 32 bytes.

```text
offset size field
0      1    schema = 1
1      1    opcode
2      1    args_len                  // 0..16
3      1    flags/reserved
4      8    command_id
12     4    expected_revision
16     N    args                      // N = args_len
```

`command_id` is a random or authority-generated 64-bit application operation
identifier. It is stable across security-level retries and gateway changes for
the same logical user request.

A 64-bit command ID is correlation/idempotency identity, **not** a nonce.

### 14.2 RESULT

```text
offset size field
0      1    schema = 1
1      1    result_code
2      1    detail_len                // 0..16
3      1    flags/reserved
4      8    command_id
12     4    resulting_revision
16     N    detail
```

Initial semantic result classes should distinguish at least:

- `APPLIED`;
- `ALREADY_SATISFIED`;
- `STALE_PRECONDITION`;
- `UNAUTHORIZED_SCOPE`;
- `INVALID_ARGUMENT`;
- `BUSY`;
- `PERSISTENCE_FAILURE`;
- `UNSUPPORTED`.

`TX_DONE`, gateway receipt and relay custody are never translated into
`APPLIED`.

## 15. Relay store-forward and one-hop wrapper

The relay stores the secure inner frame byte-for-byte unchanged.

Transport dedupe key is the visible immutable tuple:

```text
(target_device_id,
 security_context,
 origin_device_id,
 authority_generation,
 security_counter)
```

A relay does not decrypt the command and does not inspect `command_id`.

Candidate v2 one-hop wrapper:

```text
offset size field
0      1    version = 0x02
1      1    type = 0x02              // V2_RELAY_FORWARD candidate
2      1    hop_count = 1
3      1    inner_len                // 48..80
4      8    relay_device_id
12     2    ingress_rssi_dbm
14     1    ingress_snr_db
15     1    reserved = 0
16     N    exact unchanged SECURE_APP
```

Maximum relayed frame = **96 bytes**.

A relay never wraps another v2 relay wrapper. A receiver rejects nested relay
wrappers. This preserves the current cooperative one-RF-relay-hop model.

The relay wrapper is transport/path metadata, not command authority. The target
accepts the command only from the authenticated inner frame.

A malicious RF participant can still drop, delay or attempt amplification. The
bounded dedupe/rate/admission policy remains required; this draft does not claim
Byzantine routing protection.

## 16. Relay retention and RESULT cleanup

Initial safe rule:

- relay keeps a bounded persistent pending-command queue;
- duplicate custody of the same transport key does not create a second entry;
- queue overflow uses an explicit bounded admission/priority policy, never
  unbounded storage;
- an unauthenticated or merely parseable RESULT never causes authoritative
  deletion;
- if the relay cannot verify a completion proof, it removes stale copies only by
  the reviewed bounded retention/queue policy;
- stale retransmission is safe at the tracker because replay + application
  idempotency reject re-execution.

The originating gateway verifies authenticated RESULT and is the user-facing
owner of completion state.

A later verifiable relay-custody receipt may optimize cleanup, but it is not a
prerequisite for the first secure command path and is not invented here.

## 17. Mixed-firmware behavior

- v1 codecs/golden fixtures remain byte-identical.
- A v1-only node rejects/ignores version 2; it never interprets v2 as a v1
  command.
- A secure command has **no plaintext v1 fallback**.
- A v1-only relay cannot be used for the v2 secure store-forward path.
- Upgraded nodes keep dedicated v1 decoders while the owner chooses the
  development cutover period.
- Unknown v2 security contexts, required app families or reserved flags fail
  closed with no durable security/application mutation.
- Do not dual-broadcast every observation or command merely for compatibility.

## 18. Airtime / MTU budget

Reference RF remains the current measured/development profile:

- SF11;
- BW 125 kHz;
- CR 4/5;
- explicit header;
- CRC on;
- preamble 8.

Raw LoRa airtime at that profile:

| frame | bytes | airtime |
| --- | ---: | ---: |
| current v1 POSITION | 34 | ~987.136 ms |
| current v1 RELAY_FORWARD | 49 | ~1232.896 ms |
| max v2 secure inner | 80 | ~1806.336 ms |
| max one-hop v2 relay wrapper | 96 | ~2134.016 ms |

If every tracker receives **one maximum relayed command per day**, raw command
airtime is approximately:

| trackers | raw airtime/day | fraction of 24 h |
| ---: | ---: | ---: |
| 10 | 21.34 s | 0.0247% |
| 100 | 213.4 s | 0.247% |
| 1000 | 2134 s / 35.6 min | 2.47% |

This is command airtime only and does not include retries, contention, uplinks
or RESULT frames.

Commands are therefore a sparse control plane, not a polling mechanism.

At one command every 3 minutes per tracker, 1000 trackers would be physically
untenable on one SF11 channel. Existing 3-minute v1 POSITION traffic is already
well beyond one-channel capacity at that scale; the v2 command path does not
solve the broader RF-domain scaling problem.

Regional duty-cycle/install policy remains separate and must be enforced by the
RF policy layer rather than silently encoded into this protocol document.

## 19. Initial implementation slicing after approval

Approval of this design would still **not** authorize one giant implementation.

The smallest staged sequence should be:

1. host-only v2 frame codec + malformed/golden vectors + delegated KDF/nonce
   vectors;
2. RAK4631 delegated KDF/AES-CCM KAT with existing Bluefruit coexistence path;
3. reviewed tracker gateway-policy/replay persistence schema and power-cut tests;
4. reviewed gateway authority-store persistence and reserve-ahead sender tests;
5. secure receive/transmit adapter with **no side effect** beyond a test/read-only
   application request;
6. first protected desired-state config write with revision/CAS semantics;
7. RESULT path;
8. relay persistent custody/store-forward;
9. physical sleepy-tracker RX-window store-forward test;
10. only then OPEN_BLE or additional protected command families.

Do not introduce generic actuation, MESSAGE, multi-hop or Android/backend product
code inside the first codec/persistence slices.

## 20. Review gates / unresolved decisions

Independent review must challenge at least:

- whether 4 tracker gateway slots is sufficient and safely encoded;
- global gateway-policy-epoch advancement and crash ordering;
- quota-code stale-revocation bound and abuse implications;
- shared gateway sender counter across scope keys;
- exact delegated HKDF label/input bytes;
- key custody/HSM assumptions without requiring raw `K_root` export;
- gateway authority-store rollback/factory-reset behavior;
- OfflineUserGrant portability and per-gateway budget residual risk;
- 64-bit `command_id` collision/idempotency semantics;
- config revision/CAS exposure without duplicating ConfigStore ownership;
- 40-byte inner header / 80-byte inner MTU;
- one-hop wrapper/nested-wrapper behavior;
- relay retention when it cannot authenticate RESULT itself;
- 10/100/1000 airtime and queue pressure;
- malformed-frame CPU/flash DoS;
- mixed v1/v2 fail-closed behavior.

No firmware build or hardware PASS is claimed by this documentation-only draft.
