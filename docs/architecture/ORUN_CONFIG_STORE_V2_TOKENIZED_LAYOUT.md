# ORUN ConfigStore v2 Tokenized Layout Direction

Status: **PROPOSED DESIGN — DOCUMENTATION-ONLY — REVIEW REQUIRED — 2026-09-26.**

Baseline:

`main@3c79b47e5e56f84caab5e0d4bdcb7a4397925e13`

Depends on:

- `docs/architecture/ORUN_CONFIG_STATE_TOKEN_CAS_DIRECTION.md`
- `docs/audits/CONFIG_STATE_TOKEN_CAS_AUDIT_DISPOSITION.md`
- `docs/architecture/ADR_M7_PERSISTENCE_LAYOUT.md`

This slice defines the exact **on-flash ConfigStore v2 record layout**, two-page
A/B transition rules, legacy-v1 migration sequence, token-state recovery
classification and implementation invariants required before firmware code is
changed.

It does **not** implement firmware, change TLP v1, change BLE GET_CONFIG, freeze
COMMAND/RESULT application bytes or authorize production secure-RF runtime.

---

## 1. Scope and constraints

Existing ConfigStore ownership remains unchanged:

```text
0x0E9000..0x0EB000
2 x 4096-byte pages
owner: ConfigStore
```

No new partition is allocated.

The v2 design must preserve the proven family of invariants:

- erase inactive page before reuse;
- write record body + CRC first;
- program commit word last for ordinary activation;
- never corrupt the previously authoritative committed page during a normal save;
- power loss must never make config bytes and token bytes come from different
  application states;
- unchanged semantic config causes no flash write;
- token progression occurs only with a successful semantic config transition;
- legacy/tokenized migration must not resurrect an old token.

The first tokenized schema still persists exactly:

```text
tracking_interval_seconds : u32
battery_capacity_mah      : u32
```

No unrelated requested/effective/profile/capability fields are added here.

---

## 2. ConfigStore v2 exact record layout

ConfigStore v2 continues the existing `ORC1` format family and uses
`version = 2`.

All multibyte integers remain big-endian, matching existing
`journal_format::put16/put32/put64` helpers.

Record size: **48 bytes**.

```text
off  size  field
0    4     magic = 0x4F524331            // "ORC1"
4    1     version = 2
5    3     reserved = 0
8    8     storage_generation
16   2     payload_length = 20
18   2     reserved = 0

20   4     tracking_interval_seconds
24   4     battery_capacity_mah

28   8     state_incarnation
36   4     state_revision

40   4     crc32(bytes[0..39])
44   4     commit_word
```

Constants:

```text
magic              = 0x4F524331
version            = 2
payload_length     = 20
record_size        = 48
commit_word active = 0x00000000
erased word        = 0xFFFFFFFF
```

The CRC covers:

- header;
- physical storage generation;
- semantic config;
- state incarnation;
- state revision.

Therefore config and token identity are one sealed record.

### 2.1 Application state token

The durable application token is:

```text
state_token =
    state_incarnation[64]
    ||
    state_revision[32]
```

Requirements:

- `state_incarnation != 0`;
- `state_revision >= 1`;
- revision never wraps;
- token is opaque outside the config owner;
- physical `storage_generation` is not part of the application token.

The on-flash byte order above does not freeze a later RF/BLE token serialization.
Wire encoding remains owned by the later protocol slice.

### 2.2 Physical storage generation

`storage_generation` remains A/B page ordering metadata only.

Rules:

- v2 baseline begins at generation 1;
- each successful v2 semantic save increments it by one;
- it never wraps;
- two committed-valid v2 records are ordered by this field only when their
  relationship also satisfies the partition-level A/B transition invariants;
- v1 and v2 physical generations are **never compared across schema versions**.

A v1 -> v2 migration starts a new v2 physical-generation namespace at 1.

This intentionally avoids treating legacy generation as application history or
migration provenance.

---

## 3. Record states

The v2 implementation must separate **structural record state** from
**application token validity**.

At minimum one page can classify as:

```text
ERASED
V1_COMMITTED_VALID
V1_COMMITTED_INVALID
V1_UNCOMMITTED
V2_STAGED_VALID
V2_COMMITTED_VALID
V2_COMMITTED_INVALID
SUPPORTED_PARTIAL
UNSUPPORTED_OR_UNKNOWN
```

### 3.1 V2_STAGED_VALID

A v2 record is **staged** when:

- supported v2 header fields are valid;
- config semantics are valid;
- incarnation/revision are valid;
- CRC matches;
- commit word remains exactly `0xFFFFFFFF`.

A staged record is **never application-authoritative**.

Its token must never be returned as VALID.

A staged body exists because migration needs a durable copy of config/token bytes
before the last legacy page can be erased.

For ordinary v2 saves, staging is simply the existing body+CRC-before-commit
phase.

### 3.2 V2_COMMITTED_VALID

A v2 record is committed-valid only when:

- all v2 structural fields are valid;
- semantic config is valid;
- incarnation/revision are valid;
- CRC matches;
- commit word is exactly `0x00000000`.

Even then, partition-level recovery may still mark the application token
UNCERTAIN when contradictory evidence exists on the other page.

### 3.3 Partial commit word

A commit word that is neither:

```text
0xFFFFFFFF
nor
0x00000000
```

is ambiguous.

It is never treated as staged or committed-valid.

---

## 4. Partition-level recovery result

ConfigStore recovery must produce more than:

```text
config
active_page
generation
```

It must also derive:

```text
token_state =
    VALID
    UNAVAILABLE
    UNCERTAIN
```

and enough diagnostics to explain why.

The current v1 boolean `config_format::decode()` contract is not sufficient for
the v2 recovery implementation.

---

## 5. Normal v2 steady-state recovery

### 5.1 One committed-valid v2 + one erased page

Result:

```text
config       = v2 config
token        = v2 token
token_state  = VALID
active_page  = v2 page
```

### 5.2 One committed-valid v2 + one valid staged v2

The staged page never wins.

Let committed generation be `G` and staged generation be `S`.

The only normal committed+staged explanation produced by the defined save
algorithm is:

```text
S == G + 1    interrupted next save before commit
```

In that case:

```text
committed v2 remains authoritative
token_state = VALID
```

Any other relation, including:

```text
S <= G
or
S > G + 1
```

cannot arise from one valid normal v2 transition and recovery returns UNCERTAIN.

The staged page never contributes an application-valid token.

### 5.3 Two committed-valid v2 pages with different generations

Under the A/B algorithm, two simultaneously committed-valid v2 pages must be
adjacent generations.

For:

```text
G_high == G_low + 1
```

the higher generation wins and the lower page is the previous committed state.

A larger gap cannot arise from the defined normal save/re-baseline transition and
is treated as UNCERTAIN rather than blindly applying "highest wins".

### 5.4 Two committed-valid v2 pages with equal generation

This cannot arise from one valid ConfigStore transition.

Result:

```text
token_state = UNCERTAIN
```

Do not break the tie by page index.

### 5.5 Valid v2 plus contradictory committed-invalid/unknown evidence

A lower/other page that appears committed but cannot be safely interpreted may
represent a later state whose bytes were damaged.

Result:

```text
selected fallback config may remain usable
token_state = UNCERTAIN
```

Do not silently revive the older token.

---

## 6. Normal v2 semantic save

No other mutation may own the ConfigStore slot while this decision is made.

For candidate config C':

### 6.1 Semantic no-op

Semantic equality is evaluated before requiring a valid token identity, matching
the approved application CAS ordering.

If:

```text
C' == current durable semantic config
```

then, regardless of whether token state is VALID, UNAVAILABLE or UNCERTAIN:

- return synchronous no-op;
- do not erase;
- do not program;
- do not advance storage generation;
- do not advance state revision;
- token bytes/state remain unchanged.

A higher layer may report ALREADY_SATISFIED, but it may expose a usable token only
when token state is VALID.

### 6.2 Semantic change

A real semantic change through the normal save path requires:

```text
token_state == VALID
```

If token state is UNAVAILABLE or UNCERTAIN, the changed candidate is not written
through the normal save API. Establishment/recovery must first use the explicit
baseline/migration/re-baseline path so that no semantic writer can bypass token
identity.

Given current:

```text
generation = G
token      = I:R
```

require:

```text
G < UINT64_MAX
R < UINT32_MAX
```

Next state:

```text
generation = G + 1
token      = I:(R + 1)
```

Sequence:

```text
1. erase inactive page
2. encode/write v2 bytes [0..43]     // body + CRC, commit remains erased
3. read back and verify staged body
4. program commit word [44..47] = 0
5. read back and verify committed record
6. switch active page/config/token in RAM
7. publish successful save result
```

Failure before step 4:

- old active committed v2 remains authoritative;
- staged/incomplete candidate never advances token.

Failure after step 4 but before RAM publication:

- reboot recovery sees the higher committed generation;
- new config/token becomes authoritative.

This preserves reset-safe application state.

---

## 7. Fresh erased partition

When both config pages are fully erased:

- existing default config may be made available immediately as fallback runtime
  config;
- application token is initially UNAVAILABLE;
- tokenized baseline establishment may run as a bounded local persistence job.

Baseline creation:

```text
config              = defaults
storage_generation  = 1
incarnation         = fresh CSPRNG nonzero u64
revision            = 1
```

Sequence:

```text
erase target page only if required
write staged v2 body+CRC
verify
commit
verify
publish token_state = VALID
```

If CSPRNG fails:

- defaults remain usable;
- token_state remains UNAVAILABLE;
- protected CAS mutation remains unavailable;
- do not invent a deterministic incarnation.

This is a one-time persistence cost, not routine RF traffic.

---

## 8. Legacy v1 migration

### 8.1 Legacy recovery source

Automatic legacy migration is allowed only when every non-erased page can be
explained as:

- a structurally/semantically valid committed v1 record; or
- a supported v1 body whose commit word remained erased/unset and is therefore
  an uncommitted candidate.

A committed-invalid supported v1 record, a partial/ambiguous v1 commit word, or
any unsupported/unknown page is contradictory evidence and makes token state
UNCERTAIN. In particular, the current M7P5 behavior that can skip a
higher-generation semantically-invalid committed record and use a lower valid
record is **not** sufficient for tokenized migration.

When automatic migration is allowed and one or two valid committed v1 records
exist, the migration source config is the valid v1 record with the highest v1
generation.

If two valid v1 records have equal generation, recovery does not choose by page
index. Equal generation is not produced by the defined v1 A/B algorithm and is
treated as UNCERTAIN, even if the semantic values happen to match.

### 8.2 Why v2 generation restarts at 1

The first v2 migration candidate uses:

```text
storage_generation = 1
state_revision     = 1
fresh incarnation
```

Legacy and tokenized generations have different schema lifetimes and are never
compared.

This avoids:

- legacy UINT64 generation exhaustion carrying into v2;
- accidental use of legacy generation as migration provenance;
- coupling application token identity to the old A/B counter.

### 8.3 Two legacy-valid pages

The normal v1 A/B state may have both pages committed.

Migration therefore proceeds:

```text
1. identify highest-generation valid legacy source page L
2. generate fresh nonzero incarnation
3. only after CSPRNG success, erase the other/lower legacy page T
4. write v2 generation 1 / revision 1 staged record to T
5. verify staged record while commit remains erased
6. erase legacy source page L
7. verify L is erased
8. program v2 commit word on T
9. verify committed v2
10. expose token_state = VALID
```

CSPRNG failure occurs before any destructive erase whenever a valid legacy source
exists. The device therefore retains its current legacy redundancy/config and
does not spend flash wear merely to discover that a new incarnation cannot be
created.

The v2 token is not application-valid merely because step 8 programmed the
commit word. It becomes VALID only after step 9 verifies the committed record
and step 10 publishes the recovered transaction state.

### 8.4 One legacy-valid page + one erased page

Same sequence, omitting the first erase when target is already erased.

### 8.5 Why commit is delayed

The v2 body+CRC is durable before the last legacy copy is erased, but its commit
word remains erased.

Therefore migration never needs to expose a token while a committed legacy record
still exists.

No third page and no second activation marker are required.

---

## 9. Power cut during legacy migration

### 9.1 Before staged v2 body verifies

At least one valid legacy source remains.

Recovery ignores incomplete target state and retries migration with a **fresh
incarnation**.

### 9.2 After staged v2 verifies, before legacy erase begins

Recovery sees:

```text
valid legacy
+
V2_STAGED_VALID
```

The staged token is never made VALID.

The implementation may erase the staged target and restart migration from the
valid legacy source with a fresh incarnation.

### 9.3 During legacy-source erase

Possible recovery:

```text
V2_STAGED_VALID
+
non-erased/invalid former legacy page
```

This is not ordinary VALID recovery.

The staged token is never promoted.

The semantic config in the staged record may be used only as a recovery fallback
under the reviewed UNCERTAIN/re-baseline policy; a new incarnation is required
before protected CAS resumes.

A bounded recovery sequence can:

```text
1. preserve the staged semantic config as fallback
2. erase the damaged other page
3. generate fresh incarnation
4. write a fresh staged v2 baseline to the erased page with a higher local
   v2 generation than any intact staged v2 candidate
5. verify
6. erase the old staged page
7. commit the fresh candidate
8. expose VALID
```

If recovery cannot establish the preconditions safely, remain UNCERTAIN and
require explicit maintenance/re-baseline.

### 9.4 After legacy erase verifies, before v2 commit

Only an uncommitted staged v2 record may remain.

After reboot, its token still must not be assumed previously unexposed, because a
partially erased historical page can theoretically resemble an uncommitted
candidate.

Therefore reboot recovery does not simply promote the old staged token.

It uses the supported-schema re-baseline rule and creates a **fresh incarnation**
before returning to VALID.

The same rule applies to a fresh/blank-partition baseline interrupted after a
valid staged v2 body was written but before its commit word was programmed: the
staged semantic config may be reused, but that staged incarnation is never
promoted after reboot. A fresh incarnation is generated for the recovered
baseline.

### 9.5 After v2 commit verifies

Migration is complete.

The tokenized record is authoritative and token state is VALID.

---

## 10. Mixed committed legacy + committed v2

Any partition containing both:

```text
V1_COMMITTED_VALID
and
V2_COMMITTED_VALID
```

is never resolved by comparing physical generations.

An existing v2 token is never made VALID merely because its v2 generation is
higher.

This state means one of:

- interrupted/old migration implementation;
- token-unaware downgrade/write;
- unsupported manual flash history.

Recovery result begins as:

```text
token_state = UNCERTAIN
```

No existing tokenized token from the mixed state is reused.

The first implementation may auto-recover mixed committed v1+v2 only when the
two records contain the **same complete semantic config**. In that case, whichever
record was temporally newer, preserving that common semantic value cannot create
a lost config update. Recovery discards the existing v2 token identity and
creates a fresh incarnation through the reviewed migration/re-baseline path.

If the valid v1 and valid v2 semantic configs differ, current flash evidence
cannot prove which semantic state is newer:

- the v1 record may be a later token-unaware downgrade write; or
- the v2 record may represent a later state while the v1 page is stale residue
  from an unsupported/older migration history.

Therefore different-config mixed state remains UNCERTAIN and is **not**
automatically rewritten. Explicit reviewed maintenance/re-baseline must select
the semantic config before creating a fresh incarnation.

If legacy semantics are invalid/corrupt or the mixed state cannot otherwise be
safely classified, do not auto-rewrite.

---

## 11. Unsupported/newer schema

A non-erased config page that cannot be classified as supported v1 or v2 must not
be overwritten automatically merely to regain CAS availability.

Result:

```text
token_state = UNCERTAIN
protected config mutation unavailable
```

This preserves downgrade/forward-compatibility safety.

Firmware compatibility or explicit reviewed maintenance is required.

---

## 12. Config reset

With VALID v2 state:

### Config differs from defaults

Reset is a normal semantic config transition:

```text
same incarnation
revision + 1
generation + 1
config = defaults
```

and uses the ordinary v2 save sequence.

### Config already equals defaults

Synchronous no-op:

- no erase;
- no program;
- no revision change;
- token unchanged.

Config reset never resets security credentials, replay state, BLE bonds or
history.

---

## 13. Explicit re-baseline

Re-baseline is not a normal `requestSave()`.

It is a separate internal/recovery operation used only when the current config
state identity cannot remain authoritative.

A successful re-baseline:

```text
preserves the selected semantic config
creates fresh nonzero incarnation
sets revision = 1
creates a fresh v2 storage generation namespace when migrating from v1,
or a strictly newer v2 generation when recovering inside an existing v2
namespace
publishes VALID only after durable commit
```

Old application tokens then become stale.

Re-baseline must be diagnosable.

It is not a generic way to bypass STALE_PRECONDITION.

---

## 14. Tokenized ConfigStore API invariants

The implementation may choose exact C++ names later, but the following must be
structurally enforced.

### 14.1 Normal semantic mutation

A normal semantic save API:

- accepts config, not caller-supplied token/revision;
- compares full semantic config;
- internally increments revision;
- internally increments v2 storage generation;
- atomically encodes config + token;
- cannot save a changed config while keeping the old revision.

### 14.2 Read snapshot

One coherent snapshot must expose:

```text
config
token_state
token when VALID
committed-record/recovery diagnostics
```

Config and token must come from one recovery/transaction observation.

### 14.3 Mutation ownership

Only one semantic mutation/re-baseline/migration job may own ConfigStore at a
time.

Equality and token checks happen while this owner/slot is held at the
application-owner layer.

### 14.4 Special paths

Migration/re-baseline/reset use explicit paths.

Test/probe code cannot directly construct a changed semantic record with unchanged
revision.

Current test-only flash probes must use the token-aware API or be disabled for v2.

---

## 15. Decoder/classifier requirements

The v2 implementation should not replace the current narrow v1 decoder with one
ambiguous "decode anything" function.

Keep explicit versioned parsing.

Suggested logical split:

```text
classify page
  -> erased / supported-v1 / supported-v2 / unknown

decode supported v1
decode supported v2 body
evaluate commit state
validate semantic config
partition-level recovery decision
```

Do not guess unknown schema layout.

The existing v1 bytes remain readable exactly as currently encoded.

No compatibility fixture should be weakened.

---

## 16. Storage / RAM / wear budget

### Flash allocation

Unchanged:

```text
2 pages x 4096 B = 8192 B
```

v1 record:

```text
36 B
```

v2 record:

```text
48 B
```

Increase:

```text
+12 B record bytes
```

No partition expansion.

### Normal semantic save wear

Unchanged erase model:

```text
1 inactive-page erase
1 body+CRC program
1 commit-word program
```

per successful semantic change.

No write for unchanged config.

### Migration wear

One-time migration may require:

- erase old inactive legacy page;
- one staged v2 body write;
- erase last legacy source page;
- one commit write.

Interrupted migration can cost additional recovery erases/writes, but this is not
periodic operation.

### RAM

Expected new persistent/runtime state is small:

- 12-byte current token;
- token-state enum/diagnostics;
- pending 12-byte next token;
- record buffer grows from 36 to 48 bytes.

Exact compiler RAM delta must be measured in the implementation slice.

---

## 17. RF / protocol impact

This storage slice creates **no RF traffic** by itself.

It does not add token bytes to:

- POSITION;
- telemetry;
- activity;
- EVENT;
- TLP v1 packets.

It does not freeze COMMAND/RESULT bytes.

The later application/protocol slice consumes the ConfigStore snapshot/token
contract already approved in the CAS direction.

---

## 18. Required host tests for implementation

The implementation slice must add or adapt tests for at least:

### v2 codec

1. exact 48-byte golden encoding;
2. big-endian fields;
3. CRC covers bytes 0..39;
4. commit offset exactly 44;
5. reserved-byte rejection;
6. zero incarnation rejection;
7. zero revision rejection;
8. wrong payload length rejection;
9. unsupported version rejection;
10. partial/nonzero-non-FF commit rejection.

### normal v2 save/recovery

11. blank -> defaults + token baseline;
12. semantic save increments generation and revision exactly once;
13. unchanged config -> no write/no token change even when token state is
    UNAVAILABLE/UNCERTAIN;
14. changed config with UNAVAILABLE/UNCERTAIN token state cannot bypass
    baseline/re-baseline;
15. A -> B -> A -> distinct token revisions;
16. body-stage power cut -> old committed state survives;
17. post-commit/pre-result reboot -> new state recovers;
18. revision exhaustion fails closed;
19. storage-generation exhaustion fails closed.

### legacy migration

20. one valid v1 + erased target -> v2;
21. two valid v1 pages -> higher legacy config preserved;
22. v2 generation begins at 1 independent of v1 generation;
23. token never VALID before last legacy page retirement;
24. power cut before staged write completion;
25. power cut after staged verification;
26. power cut during legacy erase;
27. power cut after legacy erase before commit;
28. downgrade -> legacy write -> upgrade never resurrects old token;
29. higher-generation committed-invalid v1 + lower valid v1 -> UNCERTAIN;
30. unknown/unsupported page + valid v1 -> no automatic migration;
31. CSPRNG failure before migration does not erase a valid legacy page;
32. mixed valid v1+v2 never reuses v2 token;
33. mixed same-config v1+v2 recovery creates fresh incarnation;
34. mixed different-config v1+v2 remains UNCERTAIN;
35. unsupported schema is not overwritten.

### ambiguous recovery

36. committed-invalid higher v2 + lower valid v2 -> token UNCERTAIN;
37. equal-generation valid v2 pages -> UNCERTAIN;
38. non-adjacent committed-valid v2 generations -> UNCERTAIN;
39. committed G plus staged S remains VALID only for S == G+1;
40. staged S <= G or S > G+1 -> UNCERTAIN;
41. partial commit word -> UNCERTAIN;
42. staged v2 never exposes token as VALID after reboot;
43. interrupted fresh-baseline staged token is never promoted after reboot;
44. non-erased no-valid-record -> UNCERTAIN;
45. supported-schema re-baseline creates fresh incarnation;
46. re-baseline diagnostics are observable.

### API ownership

47. normal caller cannot supply revision;
48. semantic mutation cannot retain old revision;
49. reset changes token only when semantic config changes;
50. test/probe mutation cannot bypass token progression.

---

## 19. Physical validation boundary

Host fault tests can establish software state-machine behavior against a flash
model.

They do not prove real nRF52840 power-cut behavior.

The implementation closure must separately decide the smallest physical test that
can validate:

- staged write -> power cut;
- legacy retirement -> power cut;
- commit-last recovery;
- BLE/SoftDevice flash-gate coexistence if the runtime path uses it.

Do not report those as physically validated until performed on actual hardware.

---

## 20. Explicit non-goals

This slice does not:

- change the existing two-page partition;
- add a filesystem;
- add a third config page;
- add a second activation marker;
- persist profile/capability/service/location-source fields;
- add a generic transaction journal;
- add security/replay state to ConfigStore;
- solve external byte-for-byte snapshot rollback;
- change TLP v1;
- change BLE GET_CONFIG;
- freeze COMMAND/RESULT wire bytes;
- implement firmware.

---

## 21. Implementation order after approval

Recommended order:

```text
independent layout/recovery audit
-> exact v2 codec host tests
-> recovery/migration fault tests
-> ConfigStore v2 implementation
-> application-owner snapshot/mutation seam
-> full host/sanitizer/warnings suite
-> RAK4630 production build + RAM/flash delta
-> focused physical power-cut/SoftDevice validation
-> protected config COMMAND/RESULT wire freeze
```

Do not skip the storage/recovery evidence and jump directly to remote COMMAND
runtime.
