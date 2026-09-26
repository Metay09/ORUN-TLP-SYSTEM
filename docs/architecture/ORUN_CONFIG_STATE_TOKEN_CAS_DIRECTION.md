# ORUN Config State-Token / CAS Direction

Status: **OWNER-APPROVED DESIGN DIRECTION — INDEPENDENT AUDIT PASS WITH FIXES; FIXES APPLIED, FINAL VERIFY PENDING — DOCUMENTATION-ONLY — 2026-09-26.**

Baseline: `main@fb1a47b549d18517078facc5d2d3437445277b65`.

This document defines the application-state precondition contract required before
ORUN freezes protected configuration COMMAND/RESULT plaintext bytes.

It is deliberately a small contract. The backend/gateway should own most
orchestration, retry, UI and reconciliation work. The tracker must only retain the
minimum durable state needed to reject stale protected mutations safely.

This document does **not** implement ConfigStore v2, change TLP v1, authorize a
production secure-RF command path, or freeze complete COMMAND/RESULT byte layouts.

The earlier delegated-command independent audit did not review this later CAS
contract. Its PASS disposition must not be extended to this document by
implication.

A focused independent CAS audit of branch head
`f1ec46c139a009fea0fefb0ef647cdfe45049c40` returned **PASS WITH FIXES** with
no BLOCKER/HIGH findings. The requested F1-F9 corrections are applied by the
current branch. Final focused verification of those corrections remains required
before merge/implementation/wire-freeze closure. The durable disposition is
`docs/audits/CONFIG_STATE_TOKEN_CAS_AUDIT_DISPOSITION.md`.

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
10. one application/config owner for USB, BLE, LoRa and future backend adapters;
11. one serialized config-mutation transaction at a time across every transport.

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

### 3.4 One mutation owner across every transport

Every semantic durable config writer must use the same application/config owner
and the same state-token transition rules.

That includes future:

- protected LoRa writes;
- BLE writes;
- USB/service writes;
- local maintenance/config reset;
- backend/gateway-originated writes after transport adaptation.

No path may mutate durable semantic config while leaving the current state token
unchanged, except the explicit unchanged-state no-op.

CAS admission is serialized. The application/config owner acquires the single
mutation slot **before any authoritative desired-state equality or token
evaluation** and holds ownership through durable completion plus capture of the
RESULT state fields. A second writer must receive bounded `BUSY`/retry behavior;
it must not pre-check equality/token state and queue a competing write behind the
first one.

A save already in progress therefore prevents a second adapter from returning
`ALREADY_SATISFIED` against the pre-save state. `BUSY` does not advance the
config cache and must not carry a state token that callers may treat as the
post-operation state.

This prevents two independently valid adapters from both accepting the same
precondition, and prevents an equality read from racing an already-admitted
mutation.

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
4. acquire the single config mutation slot

   unavailable:
       BUSY
       no ConfigStore write
       no cache-authoritative state token

5. while holding the slot, compare desired state with current durable semantic state

   equal:
       ALREADY_SATISFIED
       no ConfigStore write
       token unchanged
       return current token only when token state == VALID

6. require current token state == VALID

   not valid:
       STATE_UNCERTAIN / PRECONDITION_UNAVAILABLE
       no ConfigStore write
       token_valid = 0; no usable token

7. compare expected_state_token with current_state_token

   mismatch:
       STALE_PRECONDITION
       no ConfigStore write

8. validate the complete candidate

   invalid:
       INVALID_ARGUMENT / policy result
       no ConfigStore write

9. durably commit:
       desired config
       same incarnation
       state_revision + 1
   as one application-visible atomic state transition

10. only after durable success:
       APPLIED
       capture resulting_state_token from this transaction
       release mutation ownership after RESULT state fields are captured
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

This equality check is performed only while holding the mutation slot. If the
token state is not VALID, the RESULT may still report
`ALREADY_SATISFIED` for semantic equality but must report `token_valid = 0`
and must not expose a recovered/uncertain token as cache-authoritative.

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

A structurally valid supported legacy record may use a reviewed one-time
migration/baseline operation that:

- preserves the accepted config values;
- creates a fresh non-zero incarnation;
- starts `state_revision = 1`;
- commits config + token state atomically under the new schema;
- does not expose the token as VALID while an unproven committed legacy record
  can coexist with the tokenized state.

The tokenized storage slice must make migration coexistence distinguishable from
a later token-unaware firmware write. A legacy physical generation number alone
is **not** sufficient provenance because a downgraded legacy firmware can reuse
that number. An implementation must either:

- durably bind the exact migrated legacy source identity strongly enough to prove
  that any coexisting legacy record is that migration source; or
- complete power-cut-safe retirement of committed legacy records before exposing
  the new token as VALID.

If recovery sees a committed legacy record beside tokenized state and cannot
prove that exact migration relationship, token state is UNCERTAIN and the old
tokenized token is not accepted as VALID.

Once a tokenized baseline has been established, downgrade to token-unaware
firmware is not a supported state-preserving product operation. If such firmware
writes config and a later upgrade sees mixed legacy/tokenized committed state,
recovery must not silently select the old tokenized token. It enters UNCERTAIN or
performs an explicitly reviewed migration/re-baseline that creates a fresh
incarnation.

Until migration/re-baseline is durably complete, protected CAS writes remain
unavailable.

A non-erased but invalid/corrupt/unsupported legacy partition must **not** be
silently converted into a fresh valid tokenized default baseline. That condition
is UNCERTAIN until a reviewed maintenance/re-baseline action resolves it.

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

The future tokenized decoder/recovery path must expose more than the current
boolean `config_format::decode()` success/failure. At minimum it must be able to
classify page evidence as follows:

| observed page state | CAS interpretation |
| --- | --- |
| fully erased page | no record; safe |
| commit word erased/unset | incomplete/uncommitted candidate; safe to ignore |
| commit word set + supported schema + full structural/CRC/semantic validity | valid committed candidate |
| commit word set + supported schema but structural/CRC/semantic invalidity | ambiguous; UNCERTAIN |
| commit word set + unsupported/newer schema | UNCERTAIN; do not auto-overwrite |
| non-erased partition with no valid committed record | UNCERTAIN |
| lower valid committed state plus evidence of a different invalid committed state that may be newer | UNCERTAIN |
| mixed committed legacy/tokenized state without proven migration provenance | UNCERTAIN |

For two fully valid tokenized candidates, the reviewed physical-generation
ordering rule selects the latest candidate. An invalid committed candidate is
not silently skipped merely because an older valid page can still be decoded.

Safe examples therefore include:

- inactive page erased;
- inactive candidate whose commit word was never set;
- prior active committed record intact with no contradictory committed evidence.

Ambiguous examples include:

- a structurally committed state that cannot be safely interpreted;
- committed-record corruption preventing proof of which state was latest;
- an unsupported/newer committed schema;
- downgrade/recovery evidence that could otherwise resurrect an older token.

In ambiguous cases:

```text
current config fallback may be usable under existing safe policy
BUT
state_token_valid = false
AND
protected config mutation = fail closed
```

A fresh incarnation is required before CAS mutation resumes.

For ambiguous **supported-schema** corruption where the existing safe fallback
policy has selected one supported semantic config, no unsupported schema is
present, CSPRNG is healthy and there is no mixed legacy/tokenized downgrade
evidence, a later implementation may perform a bounded local automatic
re-baseline:

1. keep the selected fallback semantic config;
2. raise a persistent/diagnostic recovery indication;
3. generate a fresh incarnation;
4. durably commit one new coherent tokenized baseline;
5. expose VALID only after that commit succeeds.

This changes only config-state identity; every old expected token becomes stale.
It is an availability/recovery mechanism, not proof that the pre-corruption
semantic value was the newest value.

Automatic rewrite is **not** allowed for unsupported/newer schema or unresolved
mixed legacy/tokenized state. Those cases require firmware compatibility or an
explicit reviewed maintenance/re-baseline path.

This is the rule that prevents ordinary torn/corrupt recovery from creating an
ABA vulnerability while still giving supported-format corruption a bounded exit
from UNCERTAIN.

### 8.1 Clean external snapshot rollback is not solved here

ConfigStore alone cannot detect a byte-for-byte restore/clone of an older,
internally valid ConfigStore partition snapshot. Such a restore can resurrect an
old config and its old token with no local corruption evidence.

Therefore this CAS contract guarantees stale-write protection across normal
application concurrency, reboot, torn writes and detectable/ambiguous ConfigStore
recovery. It does **not** claim rollback resistance against an externally restored
clean old flash snapshot.

If production threat/recovery policy must resist that stronger rollback class, a
later reviewed design needs an anchor outside the ConfigStore rollback domain,
for example SecurityStore/hardware/backend-backed monotonic evidence. Do not add
that cross-store write cost speculatively to the first CAS implementation.

A firmware/DFU/backup process must not claim rollback-safe protected config if it
can restore old ConfigStore bytes without such an anchor.

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

### 9.1 Late RESULT must not regress the backend cache

An authenticated RESULT proves what the tracker reported for that command at
RESULT creation time. It does not prove that the RESULT is the newest device
observation at backend receipt time.

Therefore a late older RESULT must not blindly overwrite a cache already advanced
by a later accepted transition or state read.

For an `APPLIED` transition created from expected token T and resulting token U,
the backend/gateway cache update should itself use CAS-style logic:

```text
if cached token == T:
    cached config/token := desired config / U
else:
    record the command RESULT
    do not regress the global current-state cache
    reconcile only if needed
```

For `ALREADY_SATISFIED`, the RESULT proves the complete desired config matched
the tracker when the RESULT was created. A cache that is absent or still tied to
the command's prior observation may adopt that config/token pair **only when the
RESULT says the token is VALID**. If `token_valid = 0`, semantic equality may be
reported but no recovered/uncertain token becomes cache-authoritative. A cache
already known to have moved to a different later observation must not be blindly
replaced.

A previously empty cache may adopt a valid late RESULT and still be temporally
stale relative to an even later tracker transition. That is acceptable for
safety: the tracker CAS check remains authoritative and will reject a later stale
mutation. Backend/gateway cache is never promoted to target authority.

For `STALE_PRECONDITION`, a returned token alone is not enough to replace the
cached config/token pair; obtain a correlated authenticated config/state
observation before treating a new pair as current.

This rule lives in backend/gateway orchestration and adds no tracker RF/storage
work.

### 9.2 Bounded authenticated config/state read

Stale-cache reconciliation must be realizable within the existing 32-byte
protected-plaintext ceiling; it must not depend on an oversized RESULT.

The later wire-contract slice must define a side-effect-free authenticated
`CONFIG_STATE_READ` operation under the delegated config authority. It may be a
read opcode carried by the COMMAND/RESULT application family, but it is not a
desired-state mutation and does not need persistent command-id idempotency.

For this read-only response, exact attempt correlation by authenticated
`request_counter` is sufficient; the read-specific RESULT schema may omit the
8-byte `command_id`. The required budget is therefore:

```text
control/schema/status/flags   <= 4 bytes
request_counter                 8 bytes
state_token                    12 bytes
current M7P5 config             8 bytes
---------------------------------------
maximum                         32 bytes
```

The flags/control budget must include token-validity semantics. When token state
is not VALID, returned token bytes are absent or explicitly non-authoritative
according to the later exact schema.

The config snapshot and token must be captured as one coherent application-state
observation. The simplest first implementation returns `BUSY` if a config
mutation is in progress rather than combining pre-commit config with
post-commit token state.

This read is required only for stale/unknown reconciliation. A normal online or
offline current-cache config mutation still performs no read-before-write RF
round trip.

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

The tokenized ConfigStore API must also make semantic mutation and token
progression structurally inseparable. A caller must not be able to invoke a
semantic `requestSave()`-equivalent that changes config while leaving revision
unchanged or supplying an arbitrary revision. The normal mutation API owns the
revision increment internally; reset, migration and re-baseline use explicit
separate reviewed paths.

Current test/probe writers, including flash probes that temporarily save a
different config and later restore it, must use the token-aware mutation API once
the tokenized schema exists or be disabled for that schema. Test-only code is not
allowed to create an application state transition that bypasses token
progression.

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

This token decision does **not** authorize a larger delegated secure frame.

The first config mutation COMMAND budget is exact at the design level:

```text
schema/opcode/args_len/flags     4 bytes
command_id                        8 bytes
expected_state_token             12 bytes
current M7P5 complete config      8 bytes
-----------------------------------------
total                            32 bytes
```

The first config mutation RESULT budget with a valid token is:

```text
schema/result_code/flags          3 bytes
command_id                        8 bytes
request_counter                   8 bytes
current/resulting_state_token    12 bytes
-----------------------------------------
subtotal                         31 bytes
remaining optional detail         1 byte
```

Therefore the first schema has at most **1 byte** of optional detail when a valid
token is present. It must not grow a variable diagnostic payload inside this
family. `BUSY`, token-invalid and other compact results may use the final
versioned schema's validity/status bits, but they do not authorize a larger
frame.

Any future semantic config expansion beyond the current 8 bytes requires a new
versioned representation/family or another explicitly reviewed packing decision;
it must not silently overflow this first 32-byte contract.

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
- `BUSY`;
- `STALE_PRECONDITION`;
- token unavailable/uncertain;
- invalid candidate/policy rejection;
- durable-storage failure;
- transport timeout remains `UNCONFIRMED`, not false application failure.

Only an authenticated tracker RESULT may establish user-visible application
outcome.

For mutation RESULTs, any state token reported as usable must be the token
captured under that transaction's mutation ownership. `BUSY` must not advance
the cache. `ALREADY_SATISFIED` may carry the current token only when token state
is VALID; otherwise the RESULT marks token validity false and the token is
non-authoritative/omitted.

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
20. stale offline cache cannot silently overwrite newer tracker state;
21. concurrent writers cannot both accept the same token before one commits;
22. desired-state equality while another save owns the slot returns BUSY rather
    than ALREADY_SATISFIED against pre-save state;
23. every semantic write source advances the same token state;
24. test/probe writers cannot bypass token progression;
25. valid legacy migration succeeds without changing semantic config;
26. power loss during legacy -> tokenized migration never exposes an unproven
    mixed record set as VALID;
27. token-aware -> token-unaware downgrade -> legacy write -> upgrade never
    resurrects the old token;
28. corrupt/non-erased legacy state does not auto-baseline as fresh;
29. commit-word-erased torn candidate is distinguished from committed-invalid
    evidence;
30. higher/contradictory committed invalid evidence yields UNCERTAIN rather than
    silently reviving a lower token;
31. unsupported/newer schema is not auto-overwritten;
32. supported-schema UNCERTAIN recovery can re-baseline only under the bounded
    conditions in §8 and creates a fresh incarnation;
33. ALREADY_SATISFIED with token state not VALID returns no cache-authoritative
    token;
34. BUSY cannot advance backend/gateway current-state cache;
35. CONFIG_STATE_READ captures one coherent config/token pair and fits the
    32-byte protected plaintext ceiling;
36. mutation RESULT with a valid token fits the 31-byte fixed budget plus at
    most one detail byte;
37. clean external snapshot rollback is documented as outside ConfigStore-only
    detection rather than falsely reported as solved.

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

1. focused final verification of the applied independent-audit corrections;
2. ConfigStore tokenized-schema exact layout design;
3. host migration/recovery/fault tests;
4. ConfigStore implementation with token validity state;
5. application-owner read/write seam including bounded CONFIG_STATE_READ;
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
