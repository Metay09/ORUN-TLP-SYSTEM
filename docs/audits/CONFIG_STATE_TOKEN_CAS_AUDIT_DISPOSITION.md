# Config State-Token / CAS Audit Disposition

Status: **FOCUSED INDEPENDENT AUDIT DISPOSITION — PASS WITH FIXES; FINAL VERIFY PASS WITH MINOR DOC FIX; R1 APPLIED — DOCUMENTATION-ONLY.**

Audit target branch:

`design/config-state-token-cas-contract`

Audited head:

`f1ec46c139a009fea0fefb0ef647cdfe45049c40`

Baseline:

`main@fb1a47b549d18517078facc5d2d3437445277b65`

Primary design source:

`docs/architecture/ORUN_CONFIG_STATE_TOKEN_CAS_DIRECTION.md`

This disposition preserves only durable review conclusions and accepted corrective
actions. Raw reviewer prompts/transcripts are intentionally not retained as
architecture sources.

---

## 1. Verdict

Independent focused review verdict:

**PASS WITH FIXES**

Severity summary:

- BLOCKER: 0
- HIGH: 0
- MEDIUM: 4
- LOW / DOC: 5

The reviewer accepted the main architecture direction:

- opaque config state token: **96 bits / 12 bytes**;
- logical structure: **64-bit incarnation + 32-bit state revision**;
- tracker-final stale-precondition authority;
- backend/gateway-heavy orchestration;
- A -> B -> A token non-reuse;
- no periodic token overhead in POSITION/telemetry/event traffic;
- current-cache mutation without an extra read-before-write LoRa round trip;
- ConfigStore ownership for token persistence;
- no generic tracker command journal for the first desired-state config family.

The review did not authorize wire freeze, production runtime, firmware changes or
physical validation.

---

## 2. MEDIUM finding disposition

### F1 — token-unaware downgrade can resurrect an old token

**Accepted.**

Failure mode:

A tokenized record may remain on one A/B page while downgraded legacy firmware,
which does not understand the tokenized schema, writes a new legacy record on the
other page. On later upgrade, blindly selecting the older tokenized record can
make an old token VALID again.

Applied direction:

- token-unaware downgrade is not a supported state-preserving operation after a
  tokenized baseline exists;
- legacy physical generation and byte-for-byte legacy record identity are both
  insufficient migration provenance because the deterministic legacy format can
  be reproduced after downgrade;
- the first tokenized implementation must power-cut-safely retire committed
  legacy records before exposing a migrated tokenized baseline as VALID;
- any committed legacy + tokenized mixed state never makes an existing tokenized
  token VALID;
- unresolved mixed legacy/tokenized state becomes UNCERTAIN or enters an
  explicitly reviewed fresh-incarnation migration/re-baseline path;
- power loss during retirement may repeat migration with a fresh incarnation
  because the not-yet-exposed tokenized identity was never established as VALID.

Required implementation test includes downgrade -> legacy write -> upgrade.

### F2 — recovery classification / UNCERTAIN exit was underspecified

**Accepted.**

Applied direction:

The future tokenized decoder/recovery path must distinguish at least:

- fully erased page;
- commit word erased/unset;
- valid committed supported record;
- committed supported record with structural/CRC/semantic invalidity;
- unsupported/newer committed schema;
- non-erased partition with no valid committed record;
- lower valid record plus contradictory invalid committed evidence;
- unresolved mixed legacy/tokenized committed state.

The current boolean `config_format::decode()` interface is not sufficient for
that future tokenized recovery logic.

Supported-schema ambiguous corruption may use a bounded CSPRNG-backed local
re-baseline only when:

- a supported fallback semantic config is available;
- no unsupported/newer schema is present;
- no unresolved mixed legacy/tokenized downgrade evidence exists;
- a fresh incarnation is durably committed before VALID is exposed;
- recovery is diagnosable.

Unsupported/newer schema is never auto-overwritten.

### F3 — equality check must be inside mutation ownership

**Accepted.**

Applied direction:

- the single config mutation slot is acquired before authoritative desired-state
  equality or token evaluation;
- an in-progress mutation causes another writer to receive `BUSY`;
- a second writer may not pre-check the same old token and queue behind the first;
- RESULT token state is captured under the same transaction ownership;
- BUSY does not advance backend/gateway current-state cache.

This prevents an equality read from racing a previously admitted save.

### F4 — stale reconciliation must fit the 32-byte plaintext ceiling

**Accepted.**

Applied direction:

A side-effect-free authenticated `CONFIG_STATE_READ` is now a required later
wire-contract capability.

For that read-specific response:

- exact request correlation may use authenticated `request_counter`;
- `command_id` may be omitted because the operation has no side effect and no
  persistent logical command-id idempotency requirement;
- required design budget is:

```text
control/schema/status/flags <= 4
request_counter               8
state_token                  12
current M7P5 config           8
-------------------------------
maximum                      32 bytes
```

This exception does not weaken command-id requirements for side-effecting
mutation RESULTs.

Normal current-cache config mutation still requires no read-before-write RF
round trip.

---

## 3. LOW / DOC finding disposition

### F5 — invalid token must not become cache-authoritative through RESULT

**Accepted.**

Applied direction:

- when token state is not VALID, RESULT reports token validity false;
- an uncertain/recovered token is omitted or explicitly non-authoritative;
- ALREADY_SATISFIED may still report semantic equality;
- STALE_PRECONDITION token alone never replaces the correlated config/token cache
  pair;
- a late valid RESULT may populate an empty cache but tracker-side CAS remains the
  final authority.

### F6 — review-status language was premature

**Accepted.**

Applied direction:

Architecture/governance text now records:

- focused independent audit: PASS WITH FIXES;
- no BLOCKER/HIGH;
- requested fixes applied;
- final focused verification pending.

No text claims final closure, wire freeze or implementation approval.

### F7 — delegated document duplicated an incomplete CAS ordering

**Accepted.**

Applied direction:

The shortened delegated-security CAS pseudocode is removed.

`ORUN_CONFIG_STATE_TOKEN_CAS_DIRECTION.md` §6 is the normative ordering,
including mutation ownership, token validity, equality, stale precondition and
BUSY.

### F8 — mutation RESULT byte budget needed to be explicit

**Accepted.**

Applied direction:

First mutation COMMAND design budget:

```text
control fields                 4
command_id                     8
expected_state_token          12
current M7P5 config            8
-------------------------------
total                          32 bytes
```

First mutation RESULT design budget with a valid token:

```text
schema/result_code/flags       3
command_id                     8
request_counter                8
state_token                   12
-------------------------------
subtotal                       31 bytes
optional detail max            1 byte
```

The first family cannot silently grow variable diagnostics or additional config
fields past the 32-byte ceiling. Later config expansion needs a versioned
representation/family or another explicit reviewed packing decision.

`BUSY` is now an explicit result semantic.

### F9 — token progression must be enforced structurally by ConfigStore API

**Accepted.**

Applied direction:

The future tokenized ConfigStore mutation API must make semantic config mutation
and state-revision progression inseparable.

Normal callers must not be able to:

- change semantic config without advancing revision;
- provide an arbitrary revision;
- bypass token progression through a direct save path.

Reset, migration and re-baseline use explicit reviewed paths.

Test-only/probe writers must use the token-aware mutation API or be disabled when
the tokenized schema is active.

---

## 4. Final focused verification

Focused final verification of the applied F1-F9 corrections through
`2141a6725662c14fe90a40102cfba36af7bc8602` returned:

**PASS WITH MINOR DOC FIX**

with:

- BLOCKER: 0
- HIGH: 0
- F1-F4: closed, subject only to the R1 wording correction below;
- F5-F9: closed;
- 96-bit / 64+32 direction: accepted;
- RF/storage-cost direction: accepted.

The only residual was **R1 — LOW/DOC**: the prior F1 wording still allowed a
migration-source identity option that the deterministic legacy format cannot make
non-reproducible. That option is removed. The first tokenized implementation now
must use power-cut-safe legacy retirement before exposing VALID, and mixed
legacy/tokenized state never revalidates an existing tokenized token.

R1 is applied in the current branch. No further broad audit round is required
before merge; review of the final wording/diff is sufficient.

Final verification confirmed:

1. no requested finding remains open;
2. mixed legacy/tokenized recovery cannot silently resurrect a token;
3. UNKNOWN/unsupported schema is not auto-overwritten;
4. equality/token checks occur under mutation ownership;
5. BUSY cannot advance cache state;
6. mutation RESULT and CONFIG_STATE_READ both fit the 32-byte ceiling;
7. read-only request-counter correlation is limited to the side-effect-free read;
8. invalid tokens are never exposed as cache-authoritative;
9. tokenized ConfigStore API requirements prevent semantic-save bypass;
10. no current TLP v1/BLE/ConfigStore/SecurityStore runtime or physical PASS is
    newly claimed.

---

## 5. Evidence boundary

This disposition is architectural/documentary evidence only.

It does **not** claim:

- host-test PASS;
- production build PASS;
- physical RAK PASS;
- measured RF airtime/range/power;
- implemented tokenized ConfigStore schema;
- implemented CONFIG_STATE_READ;
- implemented protected COMMAND/RESULT;
- production secure-RF authorization;
- rollback resistance against an externally restored clean old ConfigStore
  snapshot.

The latter remains an explicit product/threat boundary unless a later design adds
rollback evidence outside the ConfigStore rollback domain.
