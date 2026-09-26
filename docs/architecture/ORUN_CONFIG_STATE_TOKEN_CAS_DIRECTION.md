# ORUN Config State-Token / CAS Direction

Status: **OWNER-APPROVED DESIGN DIRECTION — DOCUMENTATION-ONLY — 2026-09-26.**

Baseline: `main@fb1a47b549d18517078facc5d2d3437445277b65`.

This document defines the application-state precondition contract required before
ORUN freezes protected configuration COMMAND/RESULT plaintext bytes.

It is deliberately a small contract. The backend/gateway should own most
orchestration, retry, UI and reconciliation work. The tracker must only retain the
minimum durable state needed to reject stale protected mutations safely.

This document does **not** implement ConfigStore v2, change TLP v1, authorize a
production secure-RF command path, or freeze complete COMMAND/RESULT byte layouts.

---

## 1. Why this slice exists

ORUN permits delayed and offline command delivery:

```text
online:
app -> backend -> gateway -> LoRa -> tracker

offline:
app -> local gateway -> LoRa -> tracker
```

A command may therefore be created against configuration state A, remain in
transport/custody, and reach the tracker only after another accepted operation has
changed the tracker to state B.

The target tracker is the only component that can authoritatively decide whether
the precondition is still current at dispatch time.

Backend-only conflict detection is insufficient because:

- Internet may be unavailable;
- gateway/backend cached state may be stale;
- LoRa/store-forward delivery may be delayed;
- another authorized path may change the target after command creation;
- transport receipt/TX completion is not application state.

The tracker-side mechanism must remain small enough that it does not become a
general transaction database or materially tax routine tracking traffic.

---

## 2. Design goals

The first protected desired-state config mutation must provide:

1. stale-write rejection at the tracker;
2. reset-safe desired-state idempotency;
3. safe A -> B -> A behavior without re-validating the first A token;
4. online and offline operation;
5. no periodic state-token RF traffic;
6. no token field added to normal POSITION/telemetry/event frames;
7. no generic persistent command-ID journal on the tracker;
8. bounded tracker RAM/flash/CPU cost;
9. explicit fail-closed behavior under ambiguous rollback/corruption;
10. one application/config owner for USB, BLE, LoRa and future backend adapters.

The token is a **precondition identity**, not authentication. Authentication and
authorization remain properties of the protected command envelope and application
policy.

---

## 3. Responsibility split

### 3.1 Backend / application

The backend/application side should own:

- user-visible desired state;
- the last authenticated device config snapshot **paired with** the token from
  the same accepted observation/result;
- command creation/correlation;
- retry policy;
- queue/coalescing policy;
- conflict presentation;
- online reconciliation;
- audit/history.

When online, this is where most complexity belongs.

### 3.2 Gateway

A gateway may cache the last authenticated config/token required for local
offline operation.

It may:

- create or relay an authorized protected desired-state command under its
  delegated authority;
- retain the expected token used for that command;
- relay authenticated RESULT;
- report stale-precondition conflict locally.

It must not invent a fresher token or treat RF delivery as configuration success.

### 3.3 Tracker

The tracker owns only the final authoritative check:

```text
desired state already current?
        |
       yes -> ALREADY_SATISFIED
        |
       no
        v
state token valid and expected token matches?
        |
       no -> STALE_PRECONDITION / STATE_UNCERTAIN
        |
       yes
        v
validate candidate
        |
durably commit config + next token
        |
        v
APPLIED + resulting token
```

This keeps the tracker authoritative without turning it into a backend.

---

## 4. Token width and logical model

The config state token is an **opaque 96-bit value (12 bytes)**.

The initial logical model is:

```text
state_token = incarnation[64] || state_revision[32]
```

This logical composition defines uniqueness semantics. Exact persistent byte order
and complete COMMAND/RESULT field placement remain later implementation/wire
decisions.

### 4.1 Incarnation

`incarnation`:

- is non-zero;
- is created from the platform CSPRNG when a new config-token namespace is
  established;
- remains stable across ordinary semantic config writes;
- changes when a new token namespace is deliberately established;
- must not be derived solely from boot count, wall clock, device role, config
  bytes or the existing ConfigStore physical generation.

CSPRNG failure while a new incarnation is required means token establishment
fails closed. Existing valid configuration may continue operating, but protected
CAS mutation is unavailable until a safe token namespace exists.

The token is not an authentication secret. The random incarnation exists to avoid
accidental namespace reuse across reset/re-baseline events; authenticated command
security remains mandatory.

### 4.2 State revision

`state_revision`:

- starts at 1 for a new incarnation;
- increments by exactly one for each successfully committed semantic config
  change;
- does not advance for rejected candidates;
- does not advance for failed/torn saves;
- does not advance for an unchanged desired-state no-op;
- never wraps.

If `state_revision == 0xFFFFFFFF`, a protected config mutation must fail closed
rather than wrap. Reaching this condition is far beyond realistic ConfigStore
flash endurance; no complex rollover machinery is justified in the first design.

### 4.3 Why the existing ConfigStore generation is not the token

Current `ConfigStore::generation_` is physical A/B recovery ordering state.

It may be affected by:

- storage schema/migration choices;
- page recovery;
- future repair/compaction policy;
- fallback behavior.

Application CAS identity must remain conceptually separate from physical storage
generation.

Therefore:

```text
ConfigStore physical generation != application state token
```

An implementation may store both in the same atomically committed record, but it
must not silently expose the current physical generation as the application token.

---

## 5. Token validity is explicit

The application owner must distinguish at least:

```text
VALID
UNAVAILABLE
UNCERTAIN
```

### VALID

A durable config state and its token are known to belong to one coherent committed
state.

Protected CAS mutation is allowed subject to authentication/authorization and
candidate validation.

### UNAVAILABLE

No token namespace has yet been safely established, for example during controlled
first migration from the legacy token-less schema.

Normal local operation may continue using the current configuration. Protected
remote CAS mutation is not allowed until a tokenized baseline is durably created.

### UNCERTAIN

Recovery found evidence that an older state may have been resurrected or that the
latest committed state cannot be established unambiguously.

Examples include an unsupported committed future schema, ambiguous committed
corruption or rollback-like recovery where an older otherwise-valid state could
be selected after loss of a later committed state.

The tracker may continue only under the separately defined safe config fallback
policy, but the recovered old token must **not** become valid again for protected
CAS mutation.

A maintenance/re-baseline path must create a fresh incarnation before protected
mutation resumes.

---

## 6. CAS application order

For the first protected desired-state configuration operation, the tracker must
evaluate in this order:

```text
1. authenticate/authorize protected command
2. establish replay/freshness acceptance
3. parse bounded desired state
4. compare desired state with current durable semantic state

   equal:
       ALREADY_SATISFIED
       no ConfigStore write
       token unchanged

5. require current token state == VALID

   not valid:
       STATE_UNCERTAIN / PRECONDITION_UNAVAILABLE
       no ConfigStore write

6. compare expected_state_token with current_state_token

   mismatch:
       STALE_PRECONDITION
       no ConfigStore write

7. validate the complete candidate

   invalid:
       INVALID_ARGUMENT / policy result
       no ConfigStore write

8. durably commit:
       desired config
       same incarnation
       state_revision + 1
   as one application-visible atomic state transition

9. only after durable success:
       APPLIED
       return resulting_state_token
```

This ordering is intentional.

For the first protected ConfigStore family, `desired state` means the complete
current ConfigStore semantic value, not a blind single-field patch. On the
current M7P5 schema that is exactly:

```text
tracking_interval_seconds
battery_capacity_mah
```

Encoding the complete 8-byte semantic state makes equality, retry and CAS
behavior deterministic and fits the existing protected-plaintext budget. A later
expanded config schema may define a new versioned desired-state representation;
it must not silently reinterpret this first family.

### 6.1 Why equality comes before token mismatch

Assume a command was applied successfully but its RESULT was lost.

The sender retries the same logical desired-state command with the old expected
token.

The tracker now already contains the desired state.

It returns `ALREADY_SATISFIED` without another flash write even though the old
expected token no longer matches.

This gives reset-safe desired-state idempotency without a persistent command-ID
journal.

### 6.2 A -> B -> A

Example:

```text
A: config=30 min, token=I:1
B: config=60 min, token=I:2
A: config=30 min, token=I:3
```

The second A must not resurrect token `I:1`.

Value equality does not imply state identity.

---

## 7. Initialization, migration and reset semantics

### 7.1 Existing token-less ConfigStore record

The first token-aware implementation must not pretend that an existing legacy
record already has a CAS token.

It must use a reviewed one-time migration/baseline operation that:

- preserves the accepted config values;
- creates a fresh non-zero incarnation;
- starts `state_revision = 1`;
- commits config + token state atomically under the new schema;
- exposes the token as VALID only after that commit succeeds.

Until then, protected CAS writes remain unavailable.

### 7.2 Fresh/erased config partition

A genuinely erased/fresh partition may establish a durable default baseline with
a new incarnation and revision 1.

This is a one-time/local persistence cost, not periodic RF traffic.

### 7.3 Ordinary config reset

Config reset remains a config operation, not a security reset.

If reset changes semantic config:

- keep the same incarnation;
- increment state revision;
- atomically commit defaults + next token.

If config already equals defaults:

- no write;
- token unchanged.

### 7.4 Factory/re-baseline boundary

A reviewed operation that intentionally destroys/recreates config-state identity
must establish a **new incarnation**.

Old expected tokens must never become valid merely because the same default values
appear again.

### 7.5 Power loss during a save

The existing ConfigStore A/B principle remains:

- a torn inactive candidate never replaces the last valid committed state;
- state token must be committed under the same authority/activation boundary as
  its config state;
- config and token must never become visible from different commits.

If recovery can prove the newer candidate never became authoritative, the prior
committed token remains valid.

If recovery cannot prove that, token validity becomes UNCERTAIN.

---

## 8. Recovery / rollback rule

The important distinction is not merely "can some config be decoded?"

The question for CAS is:

> Can the firmware prove that this config/token pair is still the latest
> authoritative application state?

Safe examples:

- inactive page erased;
- inactive candidate torn before activation/commit;
- prior active committed record intact.

Ambiguous examples:

- a structurally committed newer state is present but cannot be safely
  interpreted;
- committed-record corruption prevents proving which state was latest;
- downgrade/recovery would otherwise select an older token-bearing state after
  evidence of a later committed state.

In ambiguous cases:

```text
current config fallback may be usable under existing safe policy
BUT
state_token_valid = false
AND
protected config mutation = fail closed
```

A fresh incarnation is required before CAS mutation resumes.

This is the rule that prevents recovery from creating an ABA vulnerability.

---

## 9. Backend/gateway cache behavior

A cached token is an optimization, not authority.

A cached config snapshot and token are one correlated pair. Backend/gateway must
not combine a config snapshot from observation A with a token learned from
observation B and then treat the pair as current.

### Online

Typical successful flow:

```text
backend has authenticated config + token T
-> COMMAND(desired, expected=T)
-> tracker applies
-> RESULT(APPLIED, token=U)
-> backend stores U
```

No extra read-before-write RF exchange is required when the cache is current.

### Offline

Typical local flow:

```text
phone/local app
-> enrolled gateway with cached authenticated token T
-> COMMAND(desired, expected=T)
-> tracker applies or rejects
-> authenticated RESULT
```

No Internet connection is required for the token comparison itself.

### Stale cache

If the gateway/backend uses stale token T:

```text
tracker current token = U
-> STALE_PRECONDITION
```

The sender must reconcile current state before replacing the expected token.
A stale RESULT may carry the authenticated current token, but token alone does
not tell the caller what semantic fields changed. If its config snapshot is not
known current, it performs an explicit authenticated state read before deciding
whether to retry.

This conflict-path read is intentionally **not** required on the normal
current-cache success path.

The first implementation must **not** blindly convert a stale failure into
"retry with whatever token the tracker just reported", because that would erase
the concurrency protection CAS exists to provide.

A later backend policy may explicitly own authoritative desired-state
reconciliation, but that is separate from the tracker contract.

---

## 10. RF / power / storage budget

The state-token mechanism must not become routine network overhead.

### 10.1 RF rule

The 12-byte token may appear only where state synchronization requires it, such
as:

- authenticated config/state read;
- protected desired-state config COMMAND;
- authenticated config RESULT.

It is not added to:

- TLP v1 POSITION;
- routine future compact POSITION;
- routine telemetry;
- activity;
- normal EVENT/alarm traffic;
- relay forwarding solely because a node supports CAS.

Compared with the earlier provisional 32-bit COMMAND precondition field, a
96-bit token adds **8 bytes** to that COMMAND field. RESULT also needs the
96-bit current/resulting token when the config operation reports state.

Exact airtime impact must be recalculated in the later wire-freeze slice against
the final plaintext/frame sizes. No claim is made here that it is zero.

The product constraint is stronger:

> A current-token successful config write must not require an extra LoRa
> read-before-write round trip.

An extra reconciliation round trip is acceptable only when cached state is
actually stale/unknown.

### 10.2 Tracker CPU/RAM rule

The normal tracker operation is only:

- bounded token/state comparison;
- bounded config comparison/validation;
- one durable config transition when state actually changes.

No background CAS task, transaction database or unbounded history is permitted.

### 10.3 Flash rule

Token state should live under ConfigStore ownership and the existing config
partition unless a later implementation review proves that impossible.

Do not allocate a new flash partition merely for the token.

The future schema must keep config + token atomic and retain the existing
erase-before-write/commit-last power-cut family.

---

## 11. GET_CONFIG / application-state exposure

Current M7P7F BLE `GET_CONFIG` wire bytes are frozen for that milestone and must
not be silently extended.

The application model may later expose:

```text
config
state_token_valid
state_token[12]
```

but transport adaptation must respect already-frozen contracts.

Possible later choices include:

- a versioned extended config response;
- a new authenticated state-read operation;
- a protected COMMAND/RESULT family response.

This document does not select those wire bytes.

USB, BLE, LoRa and backend adapters must still converge on one application/config
owner rather than creating transport-specific CAS systems.

---

## 12. COMMAND / RESULT impact

This slice resolves one previously-open question:

**config state-token width = 96 bits / 12 bytes.**

The complete COMMAND/RESULT plaintext layouts remain unfrozen.

This token decision does **not** authorize a larger delegated secure frame. The
first config command/result contract must fit the existing 32-byte protected
plaintext ceiling. With the current M7P5 config's two 32-bit semantic fields,
the COMMAND candidate uses 24 fixed bytes plus 8 config bytes = 32 bytes. A
compact RESULT can also fit within 32 bytes if the final schema/code/flags
prefix remains bounded; the wire-freeze slice must prove the exact offsets and
leave out optional detail rather than silently increasing the frame ceiling.

The first RESULT should not echo the full config merely to avoid a later
conflict-path read; preserving the 32-byte ceiling and sparse RF use is preferred.
When reconciliation is actually needed, use the dedicated authenticated
config/state read path.

The later wire-contract slice must provide at least:

### COMMAND concept

```text
command_id
desired-state opcode/args
expected_state_token[12]
```

### RESULT concept

```text
command_id
request_counter correlation
result_code
current/resulting_state_token[12] when meaningful
bounded detail
```

Required result semantics include at least:

- `APPLIED`;
- `ALREADY_SATISFIED`;
- `STALE_PRECONDITION`;
- token unavailable/uncertain;
- invalid candidate/policy rejection;
- durable-storage failure;
- transport timeout remains `UNCONFIRMED`, not false application failure.

Only an authenticated tracker RESULT may establish user-visible application
outcome.

---

## 13. Validation required before implementation closure

The implementation slice must include host fault/recovery tests covering at least:

1. fresh tokenized baseline;
2. normal A -> B transition;
3. A -> B -> A token non-reuse;
4. unchanged desired-state no-op;
5. stale expected token rejection;
6. RESULT-loss retry -> ALREADY_SATISFIED with no second flash write;
7. invalid candidate -> token unchanged;
8. flash erase/program failure -> prior config/token retained;
9. power loss before activation -> prior token retained;
10. power loss after complete activation -> new token recovered;
11. ambiguous committed corruption -> token UNCERTAIN;
12. unsupported newer committed schema -> no old token resurrection;
13. config reset to defaults;
14. reset when already defaults -> no-op;
15. migration from current token-less ConfigStore schema;
16. revision exhaustion -> fail closed;
17. CSPRNG failure while new incarnation is required -> protected mutation unavailable;
18. online current-cache path requires no read-before-write round trip;
19. offline gateway path works without backend reachability;
20. stale offline cache cannot silently overwrite newer tracker state.

Host PASS does not imply RAK physical PASS.

The later implementation must separately report:

- host/property/fault tests;
- production build result;
- flash/RAM delta;
- physical power-cut/reset evidence where required;
- RF/power measurements only when physically performed.

---

## 14. Explicit non-claims

This design does not:

- change current ConfigStore code or schema;
- claim current ConfigStore generation is a valid CAS token;
- freeze full COMMAND/RESULT bytes;
- change M7P7F GET_CONFIG bytes;
- add periodic RF traffic;
- add token data to POSITION/telemetry/event frames;
- implement delegated secure RF;
- authorize OPEN_BLE, RF configuration, actuation, DFU or MESSAGE mutation;
- prove physical flash/power/RF behavior.

It defines the minimum state-precondition contract those later slices must obey.

---

## 15. Implementation sequence after this design

Recommended order:

1. independent review of this state-token/CAS direction;
2. ConfigStore tokenized-schema exact layout design;
3. host migration/recovery/fault tests;
4. ConfigStore implementation with token validity state;
5. application-owner read/write seam;
6. production RAK build and storage-budget verification;
7. focused physical reset/power-cut validation;
8. only then freeze the first protected config COMMAND/RESULT plaintext bytes;
9. integrate the authenticated delegated secure transport in a separate reviewed slice.

This preserves the project rule:

```text
storage/state correctness
-> application CAS contract
-> exact command/result wire
-> secure transport integration
-> physical RF/power validation
```
