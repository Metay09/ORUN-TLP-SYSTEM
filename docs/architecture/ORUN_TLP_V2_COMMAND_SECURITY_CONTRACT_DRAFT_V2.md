# ORUN TLP v2 delegated command security contract — DRAFT V2

Status: **POST-AUDIT REDESIGN CANDIDATE — NOT OWNER-APPROVED, NOT WIRE-FROZEN, NO PRODUCTION RUNTIME AUTHORIZED.**

Baseline: `main@e2a370510c595c5f4b88e94a1212fb95d848a273`.

Supersedes the first draft at
`docs/architecture/ORUN_TLP_V2_COMMAND_SECURITY_CONTRACT_DRAFT.md`.

Audit source:
`docs/audits/TLP_V2_COMMAND_SECURITY_CONTRACT_INDEPENDENT_AUDIT.md`
(targeted old draft `3af79af8a52de779cedcfc05fc76c41e91fea5b5`).

This revision incorporates the independent audit's implementation-blocking
findings while preserving the product requirement:

```text
authorized user
-> enrolled gateway
-> final self-contained protected command
-> optional opaque relay custody
-> sleepy tracker later receives the same protected object
-> tracker authenticates / replay-checks / applies
-> authenticated RESULT
```

Internet may disappear after command creation. The user may leave. The
originating gateway need not remain RF-adjacent once custody is transferred.

This is still a design artifact. It does not modify firmware, TLP v1 bytes,
SecurityStore, ConfigStore, BLE behavior or RF runtime.

---

## 1. System invariants

Keep independent:

```text
Role
!= Location Source
!= GNSS Power
!= Capability
!= Enabled Service
!= Transport
!= Device Identity
!= Profile
!= User Identity
!= Security Authority
```

And:

```text
Replay
!= Freshness
!= Nonce safety
!= Idempotency
!= Delivery
!= RESULT
```

Further rules:

- backend is canonical owner of user authorization and gateway enrollment;
- tracker keeps no user/phone ACL;
- relay is opaque transport/custody by default;
- no fleet-wide/group command-authentication key;
- BLE bond/PIN is not ORUN authorization;
- unauthenticated input never mutates durable security state;
- no protected operation falls back to plaintext TLP v1;
- accepted replay state may never be silently discarded such that an old command
  becomes valid again;
- TX_DONE, gateway receipt and relay custody never mean APPLIED.

---

## 2. Engineering scale

Design/load validation uses:

- ~10 devices baseline;
- ~30-50 medium-load case;
- ~100 devices in one RF collision domain as the current upper engineering
  stress case.

1000 devices is not a flat-RF product target. If that scale ever becomes real,
use explicit RF/domain/gateway partitioning.

The command plane is sparse. Capacity analysis must include complete delivery
chains, not only one frame.

---

## 3. Security contexts

Four secure contexts are conceptually distinct:

```text
0x01 BACKEND_A2D
0x02 DEVICE_D2A
0x03 DELEGATED_GW2D
0x04 DELEGATED_D2GW
```

The first two retain the M7P6D/M7P6F direction and persistence rules.

Delegated gateway state is not silently mapped onto M7P6F's backend A2D replay
record.

---

## 4. Delegated gateway authority model

For one tracker credential lifetime, gateway command authority uses:

```text
tracker credential_id
tracker key_epoch
gateway_device_id
gateway_policy_floor
gateway_grant_generation
scope_id
quota_code
```

The gateway never receives tracker `K_root`.

The backend authority derives only the delegated secret material needed by the
specific enrolled gateway and tracker.

### 4.1 Two-level generation model

The first draft's one global `gateway_policy_epoch` is replaced.

#### gateway_policy_floor

A tracker-wide floor used only for events that intentionally invalidate the old
gateway policy set, especially gateway removal/compromise.

Properties:

- monotonically increases;
- addition of a gateway does **not** advance it when a free authorized slot is
  available;
- ordinary gateway re-enrollment does **not** advance it;
- lower floors are rejected permanently after a higher floor is committed;
- advancing the floor safely retires old replay slots.

#### gateway_grant_generation

Per-gateway monotonically increasing generation inside one policy floor.

Used for:

- re-enrollment after factory reset;
- renewal of quota;
- scope changes;
- key rotation for that gateway.

A higher generation is authenticated and durably committed before the first
command under it is dispatched.

Lower generations for that gateway are then rejected.

### 4.2 Active revocation propagation

Offline trackers cannot learn central revocation immediately.

The architecture therefore requires a future backend-authenticated security
control operation over the existing BACKEND_A2D path:

```text
GATEWAY_POLICY_FLOOR_ADVANCE
```

It is allowed to be store-forwarded through opaque relays.

A tracker commits the higher floor before any delegated command under that floor
is accepted.

A completely offline tracker that has not yet received the advance can still
accept an old delegated grant until its quota is exhausted. This residual risk
is explicit and finite in command count, not time.

The backend issues delegated material only for trackers/site ownership that the
gateway is authorized to serve.

---

## 5. Grant quota

Quota is one property of a complete gateway grant, not a per-scope property.

All scope keys belonging to the same:

```text
(tracker, gateway, policy_floor, grant_generation)
```

share:

- one sender counter;
- one quota;
- one replay HWM at the tracker.

Candidate `quota_code`:

| code | maximum fresh frame counter |
| ---: | ---: |
| 0 | 15 |
| 1 | 63 |
| 2 | 255 |
| 3 | 1023 |

Counters begin at 1.

Quota limits stale offline authority after revocation is not yet known by the
tracker.

### 5.1 Reserve-ahead

Initial gateway sender reservation block is **1**.

Reason:

- command traffic is sparse;
- block 64 could burn the whole low quota during one reboot;
- correctness and offline availability are more important than optimizing a
  storage write that occurs only when a new protected command frame is minted.

A byte-identical transport retransmission reuses the same protected frame and
does not consume another counter.

A new cryptographic attempt/retry uses:

- the same logical `command_id`;
- a new sender counter;
- new per-frame diversification salt;
- one additional quota unit.

A later gateway-platform persistence review may increase the reserve block only
with explicit wear and reboot-loss evidence.

---

## 6. Delegated key hierarchy

The tracker credential PRK remains conceptually derived from `K_root` and
`credential_id` as in M7P6D.

A delegated grant secret is candidate-derived with domain-separated HKDF inputs:

```text
grant_info =
    ASCII("ORUN-TLP-V2-GW-GRANT-v1")
    || key_epoch_be32
    || gateway_device_id_be64
    || gateway_policy_floor_be32
    || gateway_grant_generation_be32
    || scope_id_u8
    || quota_code_u8

K_grant = HKDF(..., grant_info, 32 bytes)
```

The exact extract/expand bytes require independent vectors before implementation.

### 6.1 Per-frame key diversification

A delegated frame carries a fresh 96-bit random `frame_key_salt`.

For each new cryptographic frame:

```text
frame_prk =
    HKDF-Extract(
        SHA-256,
        salt = frame_key_salt[12],
        IKM = K_grant[32])

frame_info =
    ASCII("ORUN-TLP-V2-GW-FRAME-v1")
    || direction_u8

K_frame =
    HKDF-Expand(SHA-256, frame_prk, frame_info, 16 bytes)
```

AES-CCM then uses `K_frame`.

Purpose:

- replay safety still comes from the durable monotonic sender counter/HWM;
- nonce safety no longer depends solely on rollback-proof gateway counter
  storage;
- if a stale gateway storage image accidentally reuses a sender counter, a fresh
  CSPRNG salt produces a different AEAD key;
- byte-identical retransmission retains the original salt/key/ciphertext.

Production use is blocked until the selected gateway platform has a physically
validated CSPRNG path.

A raw storage clone can still create authority/availability problems and is a
physical compromise class, but it must not silently create same-key/same-nonce
reuse if new frames use fresh salts correctly.

---

## 7. Nonce and counter ownership

### 7.1 GW2D nonce

```text
key_epoch_be32
|| 0x03
|| gateway_sender_counter_be64
```

Counter owner:

```text
(tracker, gateway_device_id, policy_floor, grant_generation)
```

All scope keys under the grant use the same counter.

### 7.2 D2GW RESULT nonce

```text
key_epoch_be32
|| 0x04
|| device_tx_counter_be64
```

The device uses its existing durable security TX allocator; the delegated
direction/key is separate.

### 7.3 Single outstanding frame per tracker

Because tracker replay admission is a strict HWM, one gateway may have only one
distinct in-flight GW2D frame per tracker.

Allowed:

- repeated byte-identical transmission of the current frame.

Not allowed:

- counter N and N+1 simultaneously sent through independent paths.

The next distinct counter is emitted only after the prior operation reaches the
gateway's defined terminal retry/RESULT state.

---

## 8. Opcode / scope binding

A delegated scope key is meaningful only if application dispatch enforces it.

Firmware therefore requires a fixed reviewed registry:

```text
opcode -> required scope_id
```

Receive order:

1. authenticate/decrypt;
2. replay commit;
3. parse command;
4. verify `opcode -> scope_id`;
5. only then call the application owner.

Mismatch returns/records `UNAUTHORIZED_SCOPE` with no application side effect.

The backend authority context may have a separately defined broader system
scope, but that is explicit and not inferred from gateway transport.

---

## 9. Tracker delegated replay persistence

Initial product limit:

**maximum 4 simultaneously command-capable gateways per tracker policy floor.**

Backend enrollment must not issue more than four active delegated command grants
for one tracker/floor.

Tracker durable security state conceptually includes:

```text
gateway_policy_floor

slot[4]:
    gateway_device_id
    gateway_grant_generation
    replay durable bound/HWM
```

Recovery rejects/faults on:

- duplicate gateway IDs in two slots;
- decreasing HWM;
- slot generation inconsistent with authoritative floor;
- corrupt/ambiguous state;
- unsupported record/schema.

No silent LRU eviction.

A fifth unknown gateway at the current floor is rejected until policy changes
free a slot.

### 9.1 Reset/reclamation rule

Delegated floor/slot state may never be reset independently while the same
tracker credential remains authoritative.

The only unconditional reset boundary is a new credential lifetime with a new
`credential_id` / `K_root`.

Therefore the future implementation belongs in the same atomic security
authority domain as the credential.

Current design direction: **SecurityStore format v3**, not a separate eraseable
replay store.

Exact bytes/records remain a later persistence slice.

---

## 10. Offline user authorization

Human authorization remains separate from GW2D command authentication.

Candidate `OfflineUserGrant` is backend-signed and gateway-independent, but it
is **not** a bearer token.

It binds at least:

```text
grant_id
user/account subject
application public-key identity/fingerprint
tenant/site/target-set reference
authorization_generation
allowed operation scopes
offline operation budget
backend signing-key id
backend signature
```

The application proves possession of the corresponding private key using a
fresh gateway challenge.

Requirements:

- use a mature standard signature primitive; no ORUN-designed signature scheme;
- prefer hardware-backed/non-exportable app keys when the platform provides
  them;
- gateway stores the highest accepted authorization generation for the user;
- lower generations are rejected;
- when Internet exists, gateway/backend checks current revocation/minimum
  generation before allowing protected operations;
- factory re-enrollment refreshes revocation state;
- the same logical command retry does not consume multiple human authorization
  budget units merely because transport retries occurred.

The tracker never sees user identity or this grant.

Worst-case stale-user authority across several already-enrolled gateways equals
the sum of the remaining bounded offline budgets. This is an explicit residual
of offline operation.

Signature suite and exact grant bytes remain a separate user-auth slice.

---

## 11. Safe receive ordering

For DELEGATED_GW2D:

1. verify bounded total length/version/type/reserved fields;
2. verify target is local device;
3. verify `security_context/app_family` combination is allowed;
4. require `key_epoch == current accepted key epoch`;
5. require policy floor is not below durable floor;
6. resolve existing gateway slot or prove one free slot is available;
7. reject grant generation below the slot's durable generation;
8. validate quota/scope codes and `counter <= quota_max`;
9. derive `K_grant` and `K_frame` from visible candidate fields;
10. perform AEAD authenticate/decrypt;
11. on authentication failure: no RF response, no flash mutation;
12. require `counter > runtime/durable replay HWM`;
13. commit any required floor/slot/HWM advance durably;
14. parse plaintext;
15. verify opcode/scope binding;
16. evaluate freshness/precondition/idempotency;
17. dispatch to the application owner;
18. emit authenticated RESULT when appropriate.

Visible routing/security fields are untrusted inputs until step 10 succeeds.

Replay rejection does not generate an RF error response.

---

## 12. Command freshness

A generic tracker wall-clock TTL is not assumed.

Initial delayed store-forward is allowed only for operations whose safety can be
expressed through durable desired-state/precondition semantics.

### 12.1 Initial allowed family

First protected mutation candidate:

**desired-state configuration write**

It uses an opaque application state precondition token.

The current `ConfigStore::generation_` is **not automatically that token**.

A separately reviewed config slice must provide a token with no unsafe ABA reuse
across recovery/fallback.

The implementation order is:

```text
if desired state already equals current durable state:
    ALREADY_SATISFIED
else if expected_state_token != current_state_token:
    STALE_PRECONDITION
else:
    validate full candidate
    durable ConfigStore save
    APPLIED
```

This ordering supports RESULT-loss retry without another flash write.

### 12.2 Delayed store-forward prohibited until separately designed

Do not permit arbitrary delayed execution yet for:

- actuation;
- FREE_GRAZE;
- LOST/SEARCH mode transitions;
- RF reconfiguration;
- OPEN_BLE;
- credential/security management;
- DFU trigger;
- MESSAGE until MESSAGE TTL/custody semantics are frozen.

Those families need explicit freshness/challenge/TTL semantics before being
store-forwardable.

---

## 13. command_id and idempotency

`command_id` is application correlation identity.

It is:

- stable across cryptographic retries;
- independent from security counters;
- not an AEAD nonce;
- not a tracker replay HWM;
- not assumed to be a persistent tracker command journal key.

For the first desired-state config operation, reset-safe idempotency comes from:

- desired state comparison;
- application precondition token;
- durable ConfigStore state.

Tracker need not persist a generic command-ID journal for this case.

Gateway/backend must reject reuse of one `command_id` for a different logical
payload/target.

Future non-idempotent actuators require their own durable application transaction
state before being enabled.

---

## 14. Secure frame candidate V2

This is an exact **candidate for re-audit**, not wire freeze.

All integers are big-endian.

### 14.1 SECURE_APP

Header size: 56 bytes.

Maximum protected plaintext: 32 bytes.

Tag: 8 bytes.

Total: 64..96 bytes.

```text
off  size  field
0    1     version = 0x02
1    1     type = 0x01                 // SECURE_APP candidate
2    1     security_context
3    1     app_family
4    1     path_flags
5    1     grant_flags
6    1     ciphertext_len              // 0..32; family minimum applies
7    1     reserved = 0

8    8     origin_device_id
16   8     target_device_id
24   4     key_epoch
28   8     security_counter
36   4     gateway_policy_floor
40   4     gateway_grant_generation
44   12    frame_key_salt

56   N     ciphertext
56+N 8     AES-CCM tag
```

AAD is exactly bytes 0..55.

### 14.2 Field rules

`path_flags`:

- bit0 = relay_allowed;
- bits1..7 = 0.

`grant_flags` for delegated contexts:

- bits0..1 = quota_code;
- bits2..5 = scope_id;
- bits6..7 = 0.

For non-delegated contexts, delegated-only fields must be zero according to that
context's later frozen layout.

Invalid/reserved values fail closed.

### 14.3 Context/family matrix

Initial delegated matrix:

```text
DELEGATED_GW2D -> COMMAND
DELEGATED_D2GW -> RESULT
```

Other combinations reject.

Backend/device contexts get their own reviewed family matrix rather than being
implicitly accepted.

---

## 15. COMMAND candidate

Minimum plaintext length: 16 bytes.

```text
off size field
0   1    schema = 1
1   1    opcode
2   1    args_len
3   1    flags
4   8    command_id
12  4    expected_state_token_low32 / family-defined precondition field
16  N    args
```

This table is not yet sufficient for final wire freeze because the config
precondition token may need more than 32 bits.

Therefore the COMMAND payload layout remains **provisional** until the config
state-token slice resolves width/semantics.

Flags/reserved bits must reject unknown critical values.

---

## 16. RESULT candidate

RESULT must correlate both the logical command and the exact GW2D attempt.

Candidate minimum:

```text
schema
result_code
flags/reserved
command_id
request_counter
resulting_state_token
bounded detail
```

The exact state-token width is unfrozen with COMMAND.

RESULT uses the same delegated grant context:

```text
policy_floor
grant_generation
scope
quota
gateway identity
```

and DELEGATED_D2GW direction.

A gateway that cannot itself consume the RESULT may forward/store the opaque
RESULT for later backend delivery.

Only a successfully authenticated target RESULT can produce user-visible
`APPLIED`.

---

## 17. Relay custody

The first draft's unauthenticated persistent relay queue is removed.

### 17.1 Initial secure-command relay slice

Initial custody is:

- RAM-only;
- target-bounded;
- globally bounded;
- no flash write from an unverified opaque command.

Candidate limits for later implementation review:

- max 1-2 pending entries per target;
- max 8 total pending command entries per relay;
- full-frame/tag-aware dedupe;
- bounded delivery attempts;
- bounded relay-local retention time;
- one active custodian for one pending command.

A relay reboot may lose pending RAM custody in this first slice. That is an
availability limitation, not a security failure.

### 17.2 Persistent relay custody

Persistent opaque command custody remains a product requirement, but is deferred
until relay admission can be authenticated without giving the relay tracker root
credentials.

Candidate future mechanisms may include a reviewed gateway->relay admission
credential/MAC or equivalent bounded trust mechanism.

Do not invent it inside the secure-frame codec slice.

---

## 18. Relay wrapper candidate

```text
off size field
0   1    version = 0x02
1   1    type = 0x02
2   1    hop_count = 1
3   1    inner_len
4   8    relay_device_id
12  2    ingress_rssi_dbm
14  1    ingress_snr_db
15  1    reserved = 0
16  N    exact unchanged SECURE_APP
```

Maximum with the current 96-byte inner frame: **112 bytes**.

Rules:

- nested relay wrappers reject;
- relay metadata is untrusted path observation;
- inner `relay_allowed` must be set;
- relay may not alter the secure inner frame;
- dedupe uses a hash/full-frame identity that includes the authentication tag,
  not only predictable counter tuple fields;
- a syntactically different frame with the same visible tuple must not poison
  the legitimate frame's dedupe slot.

Multiple relays must not all transmit at the same tracker RX opportunity.

Initial topology uses a single selected custodian. A later multi-relay design
needs deterministic slot/backoff or explicit custody assignment.

---

## 19. Airtime budget

Reference profile remains:

- SF11;
- BW 125 kHz;
- CR 4/5;
- preamble 8;
- explicit header;
- CRC enabled.

Calculated raw airtime:

| frame | bytes | airtime |
| --- | ---: | ---: |
| v1 POSITION | 34 | ~987.136 ms |
| v1 RELAY_FORWARD | 49 | ~1232.896 ms |
| max secure inner V2 | 96 | ~2134.016 ms |
| max relay wrapper V2 | 112 | ~2379.776 ms |

These are calculations, not RF measurements.

A command operation must budget the entire exchange:

```text
GW -> tracker/relay
relay -> tracker when needed
tracker -> RESULT
optional relay/gateway forwarding of RESULT
retries
```

At ~100 devices, sparse command traffic is feasible only when ordinary tracking,
history, alarms and relay retransmissions are also budgeted.

No fleet-wide bulk command should be designed as a simultaneous broadcast burst.

---

## 20. Gateway storage / rollback requirements

Gateway delegated authority state is a separate persistence owner from tracker
ConfigStore/History/BLE storage.

It must durably own:

- tracker binding;
- policy floor;
- grant generation;
- delegated scope secrets;
- sender counter;
- quota;
- pending frame bytes for byte-identical retry;
- RESULT replay/correlation state;
- offline-user generation/budget state.

Automatic cloud/file backup of raw authority storage must be disabled unless the
backup/restore protocol itself is security-aware.

The future gateway-platform review must establish:

- partition/owner;
- power-cut semantics;
- wear;
- clone/restore behavior;
- factory reset;
- CSPRNG quality;
- whether hardware rollback resistance/secure element exists.

Current RAK4631 remains the firmware reference hardware, but production delegated
gateway authority is not assumed to be safe merely because tracker firmware runs
on the same board.

---

## 21. Backward compatibility

This design does not change:

- v1 TEST 18 B;
- v1 POSITION 34 B;
- v1 RELAY_FORWARD 49 B;
- current one-hop v1 behavior;
- current RF configuration;
- current HistoryStore;
- current ConfigStore;
- current BLE GATT behavior;
- current tracker RX policy.

A v1-only node must not interpret v2 bytes as a command.

No plaintext v1 command fallback exists.

---

## 22. Required implementation slicing after approval

Do not implement this as one milestone.

1. independent re-audit of this V2 design;
2. host-only delegated KDF/frame-key vectors;
3. exact SECURE_APP codec golden/malformed tests;
4. RAK delegated KDF/AES-CCM/CSPRNG KAT/coexistence proof;
5. SecurityStore v3 delegated replay design + fault tests;
6. gateway authority-store design + crash/rollback tests;
7. secure receive path to a read-only/no-side-effect test application;
8. config state-token/CAS design;
9. first protected desired-state config write;
10. authenticated RESULT;
11. RAM-only relay custody;
12. physical sleepy-tracker relay delivery;
13. persistent relay custody only after authenticated admission design.

OPEN_BLE, RF config, actuation and MESSAGE remain later families.

---

## 23. Audit finding disposition

The old independent audit remains historical evidence for `3af79af`.

This revision intentionally addresses:

- F1/F2: quota semantics and reserve block;
- F3/F4: two-level generation and active floor advance;
- F5: opcode-scope registry;
- F6: OfflineUserGrant PoP/target/generation;
- F7: per-frame key diversification + storage/CSPRNG requirements;
- F8: delegated state lifetime bound to SecurityStore/credential lifetime;
- F9-F12: no unauthenticated persistent custody, tag-aware dedupe, single
  in-flight sender and single custodian;
- F13: explicit safe receive ordering;
- F14: current ConfigStore generation not accepted as CAS token;
- F15: RESULT must bind request counter;
- F16: whole exchange airtime is budgeted;
- F17: unsafe delayed families explicitly prohibited;
- F18-F23: reserved-field/context rules, command-id semantics, gateway limit,
  metadata/resource/doc constraints.

This is a claimed **design correction**, not proof the findings are closed.
Only a new independent audit can close them.

---

## 24. Re-audit gate

Before owner approval/wire freeze, independent review must specifically verify:

- no same-key/same-nonce path remains under counter rollback/clone/reboot;
- per-frame key diversification is sound and domain-separated;
- tracker replay HWM and grant-generation transitions cannot resurrect old
  frames;
- floor advancement crash ordering;
- quota and one-in-flight sender semantics;
- scope/opcode authorization;
- OfflineUserGrant PoP and target scoping;
- SecurityStore v3 lifecycle assumption;
- RESULT correlation;
- relay RAM custody DoS bounds;
- exact 56-byte AAD header;
- 96/112-byte airtime and ~10/~30-50/~100 load behavior;
- future product families remain possible without turning this command frame
  into a monolithic universal payload.

No firmware/runtime/build/physical PASS is claimed by this document.
