# ORUN ConfigStore v2 Tokenized Layout Direction

Status: **DESIGN REVIEW COMPLETE — INDEPENDENT AUDIT PASS WITH FIXES; FINAL VERIFY PASS WITH MINOR DOC FIX; H1/H2, M1-M4, L1-L5 AND R1-R3 APPLIED — DOCUMENTATION-ONLY — 2026-09-26.**

Baseline:

`main@3c79b47e5e56f84caab5e0d4bdcb7a4397925e13`

Depends on:

- `docs/architecture/ORUN_CONFIG_STATE_TOKEN_CAS_DIRECTION.md`
- `docs/audits/CONFIG_STATE_TOKEN_CAS_AUDIT_DISPOSITION.md`
- `docs/architecture/ADR_M7_PERSISTENCE_LAYOUT.md`

This slice defines the exact **on-flash ConfigStore v2 record layout**, two-page
A/B transition rules, token-state recovery classification and implementation
invariants required before firmware code is changed.

The repository currently has **no deployed field fleet whose ConfigStore v1
contents must be preserved**. Therefore automatic v1 -> v2 migration is not a
current product requirement. The previously reviewed legacy-migration sections
remain as contingency analysis only; they are not authorized for implementation
without a new explicit product need.

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
- token progression occurs only with a successful semantic config transition.

ConfigStore v1 development data is **not** a backward-compatibility promise.
TLP v1 wire compatibility remains a separate invariant and is unaffected by this
storage cutover policy.

The first tokenized schema still persists exactly:

```text
tracking_interval_seconds : u32
battery_capacity_mah      : u32
```

No unrelated requested/effective/profile/capability fields are added here.

### 1.1 Development cutover policy

Current product phase:

```text
deployed ConfigStore fleet = none
development devices        = resettable
```

The first production-intent v2 runtime therefore uses a **clean cutover**:

```text
existing development ConfigStore v1 data
-> explicit development/maintenance ConfigStore partition erase
-> blank two-page ConfigStore
-> fresh nonzero CSPRNG incarnation
-> revision 1
-> v2 baseline
-> v2-only normal operation
```

Rules:

- do not implement automatic v1 -> v2 semantic migration in the current product;
- do not preserve development-only v1 config merely for historical convenience;
- once the runtime cutover is complete, normal firmware writes only v2;
- a later v2 runtime that encounters a committed v1 record does not auto-migrate
  it: it reports a legacy-development-schema / maintenance-reset condition;
- mixed committed v1+v2 is never auto-reconciled in the current product; it is a
  maintenance/reset condition;
- legacy v1 codec code may remain temporarily only while the current development
  runtime still writes v1, then should be removed when no longer referenced;
- if a real deployed-fleet migration requirement appears in the future, the
  reviewed contingency analysis below may be reconsidered in a separate slice
  rather than silently enabling it.

This is intentionally different from TLP v1 compatibility. Protocol byte
compatibility is still protected; development flash contents are not.

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

### 2.1 Page-local token-retire word

The v2 **record remains 48 bytes**. One additional page-local monotonic word is
reserved immediately after it:

```text
page offset 48..51  token_retire_word
```

Values:

```text
0xFFFFFFFF  token identity not explicitly retired
any other value  token identity retired / non-authoritative
```

The implementation programs `0x00000000` when intentionally retiring an
otherwise committed v2 token before destructive re-baseline work. Recovery treats
**any non-FF value** as retired so a partially programmed retire word fails safe.

The retire word is deliberately outside the v2 record CRC. It must be writable
from erased `0xFFFFFFFF` to a retired value without rewriting the committed
record.

Normal config saves never program this word. A target page is fully erased before
a new v2 record is staged, so its retire word begins at `0xFFFFFFFF`.

A retire operation is complete only after the 4-byte write has been read back and
verified as non-FF. Contradictory evidence must not be erased before that
verification succeeds.

This adds one 4-byte page-local metadata location, not a third page, filesystem,
second record activation marker or periodic flash write.

### 2.2 Application state token

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

### 2.3 Physical storage generation

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

## 3. Page classification

The v2 implementation must separate:

1. physical/page evidence;
2. supported schema decoding;
3. semantic config validity;
4. application token validity.

The current v1 boolean `config_format::decode()` result is not sufficient.

### 3.1 Fixed forward-compatibility discriminator and classification order

The ConfigStore format family keeps the fixed magic:

```text
ORC1 = 0x4F524331
```

at offset 0 and the schema version at byte 4.

A classifier must apply evidence tests in this order:

```text
1. fully erased
2. exact supported v1/v2 forms and their known commit offsets
3. recognizable local torn-write / programmed-prefix + all-FF-tail evidence
4. genuine UNSUPPORTED_NEWER discriminator
5. remaining local damage -> SUPPORTED_CORRUPT
```

This ordering is normative. A page that already matches a supported torn-write
pattern is never reclassified as a future schema merely because its partially
programmed/erased version byte is numerically unfamiliar.

Future ConfigStore schema versions must preserve:

```text
magic == ORC1
bytes[5..7] == 0
(version & 0x03) == 0
```

unless a separately reviewed classifier migration changes that contract first.
Therefore the first future-version values are drawn from the reserved
multiple-of-four namespace (for example 4, 8, 12...), not 3.

This makes a half-erased v1/v2 version byte fail toward local corruption instead
of masquerading as a future format: flash erase can change 0 bits to 1, so a
residual v1 version keeps bit 0 set and a residual v2 version keeps bit 1 set.

After the earlier supported/torn checks, `UNSUPPORTED_NEWER` requires all of:

```text
magic == ORC1
version not in {1, 2, 0xFF}
bytes[5..7] == 0
(version & 0x03) == 0
```

Such a page is never auto-overwritten by this implementation.

`ORC1 + version == 0xFF` that does not match a recognized torn-prefix pattern is
`SUPPORTED_CORRUPT`, not UNSUPPORTED_NEWER.

A non-erased page with damaged/non-ORC1 magic is likewise **not automatically
called a future schema**. It is local corruption/torn-erase evidence owned by the
known ConfigStore region.

### 3.2 Required physical/evidence classes

At minimum one page can classify as:

```text
ERASED

V1_COMMITTED_VALID
V1_COMMITTED_INVALID
V1_UNCOMMITTED_OR_TORN

V2_STAGED_VALID
V2_UNCOMMITTED_OR_TORN
V2_PARTIAL_COMMIT
V2_COMMITTED_VALID
V2_COMMITTED_INVALID
V2_COMMITTED_RETIRED

SUPPORTED_CORRUPT
UNSUPPORTED_NEWER
```

The classifier may keep finer diagnostics, but it must not collapse
`SUPPORTED_CORRUPT` into `UNSUPPORTED_NEWER`.

### 3.3 Supported v1 evidence

For exact `ORC1/version=1`:

- commit word at v1 offset 32 == `0xFFFFFFFF`:
  `V1_UNCOMMITTED_OR_TORN`; it is not authoritative even if CRC/body are
  incomplete;
- commit word == `0x00000000` and full v1 structure/CRC/semantics valid:
  `V1_COMMITTED_VALID`;
- commit word == `0x00000000` but structure/CRC/semantics invalid:
  `V1_COMMITTED_INVALID`;
- any other commit value:
  `SUPPORTED_CORRUPT`.

v1 never has an application state token.

### 3.4 Supported v2 evidence

For exact `ORC1/version=2`:

- commit word at v2 offset 44 == `0xFFFFFFFF` and the complete v2 body,
  CRC, config and token fields verify:
  `V2_STAGED_VALID`;
- commit word == `0xFFFFFFFF` but the body is incomplete/invalid:
  `V2_UNCOMMITTED_OR_TORN`;
- commit word is neither `0xFFFFFFFF` nor `0x00000000`, while the complete
  v2 body/CRC/config/token fields verify:
  `V2_PARTIAL_COMMIT`;
- commit word == `0x00000000`, record verifies and retire word is
  `0xFFFFFFFF`: `V2_COMMITTED_VALID`;
- commit word == `0x00000000`, record verifies and retire word is non-FF:
  `V2_COMMITTED_RETIRED`;
- commit word == `0x00000000` but record/config/token validation fails:
  `V2_COMMITTED_INVALID`;
- a partial commit with an invalid body/CRC/config/token, or any other supported
  malformed state:
  `SUPPORTED_CORRUPT`.

A staged or partial-commit record is never application-authoritative. Its token
is never returned as VALID.

A `V2_PARTIAL_COMMIT` record **may preserve its verified semantic config** as a
recovery safety copy. It is treated like a staged semantic copy for recovery,
but its token identity is permanently non-authoritative and a fresh incarnation
is required before CAS resumes.

A retired committed record may likewise remain the selected **semantic fallback
copy**, but its token is permanently non-authoritative until that page is erased
and a fresh incarnation is committed elsewhere.

### 3.5 Torn prefix / interrupted erase evidence

Flash programming and page erase may be interrupted between physical word
operations. The classifier must not require a complete valid magic/version merely
to recognize that a page can be local torn-write/erase residue.

A record-sized region that consists of a programmed prefix followed by an
all-`0xFF` untouched tail is treated as non-authoritative torn local evidence,
not as an unsupported future schema. This torn-prefix test is evaluated **before**
the future-version discriminator in §3.1.

Other non-erased damage, including `ORC1/version=0xFF` that does not match a
recognized prefix shape, is `SUPPORTED_CORRUPT` for recovery-policy purposes
unless it satisfies the complete `UNSUPPORTED_NEWER` discriminator. It may make
token state UNCERTAIN, but it does not by itself prohibit the bounded
supported-corruption re-baseline defined below when an unambiguous semantic
fallback exists.

### 3.6 Partial commit/retire words

A commit word that is neither:

```text
0xFFFFFFFF
nor
0x00000000
```

is never active. When the complete v2 body/CRC/config/token fields still verify,
it is `V2_PARTIAL_COMMIT` and may preserve semantic config only. Otherwise it
is `SUPPORTED_CORRUPT`.

For `token_retire_word`, the safety rule is deliberately different:

```text
0xFFFFFFFF = not retired
anything else = retired
```

because a partial retire write must fail toward **token invalidation**, never
toward resurrection.

---

## 4. Partition-level recovery result

ConfigStore recovery must produce more than:

```text
config
active_page
generation
```

It must also derive at least:

```text
token_state =
    VALID
    UNAVAILABLE
    UNCERTAIN

semantic_state =
    UNAMBIGUOUS
    FALLBACK_ONLY
    AMBIGUOUS
```

and enough diagnostics to explain why.

`UNAMBIGUOUS` means recovery policy has one complete semantic config value to
preserve: either one usable supported copy or multiple copies that agree.

`FALLBACK_ONLY` means no supported durable/staged copy can establish a semantic
config; runtime may use the existing safe defaults, but those defaults are not
presented as a recovered durable application state.

`AMBIGUOUS` means usable/possible evidence can represent different semantic
values and current flash cannot safely select one.

A protected semantic change is never allowed while token state is not VALID.
Equality/no-op under UNAVAILABLE/UNCERTAIN is allowed only for
`semantic_state == UNAMBIGUOUS`. It is not used for FALLBACK_ONLY or AMBIGUOUS.

---

## 5. Normal v2 steady-state recovery

The rules in this section apply only when no page carries a retire marker and no
other contradictory evidence is present.

### 5.1 One committed-valid v2 + one erased page

If the committed page's retire word is `0xFFFFFFFF`:

```text
config       = v2 config
token        = v2 token
token_state  = VALID
active_page  = v2 page
```

If the committed page is retired, the same config may remain an operational
fallback but:

```text
token_state = UNCERTAIN
```

A retired token is never resurrected merely because the other page later became
erased.

### 5.2 One committed-valid v2 + one valid staged v2

The staged page never wins.

Let committed state be:

```text
generation = G
token      = I:R
```

and staged state be:

```text
generation = S
token      = J:Q
```

The only normal interrupted-next-save relation is:

```text
retire_word(committed) == 0xFFFFFFFF
S == G + 1
J == I
Q == R + 1
```

Only in that case:

```text
committed v2 remains authoritative
token_state = VALID
```

Any different incarnation, non-successor revision, retired committed page or
other generation relationship is UNCERTAIN.

This specifically prevents a fresh-incarnation re-baseline stage from making an
older committed token VALID after contradictory evidence has been erased.

### 5.3 Two committed-valid v2 pages with different generations

A normal completed semantic save leaves two non-retired committed pages whose
storage/token progression is adjacent:

```text
G_high == G_low + 1
incarnation_high == incarnation_low
revision_high == revision_low + 1
retire_word(high) == retire_word(low) == 0xFFFFFFFF
```

Only under all of those conditions does the higher generation win and the lower
page represent the previous committed state.

A generation gap, incarnation change, revision discontinuity or retire marker is
not explained by the normal save path and makes token state UNCERTAIN.

### 5.4 Two committed-valid v2 pages with equal generation

Equal generation is not resolved by page index.

Result:

```text
token_state = UNCERTAIN
```

If their complete semantic configs are identical, the semantic value is
UNAMBIGUOUS and may be eligible for fresh-incarnation re-baseline. If the configs
differ, semantic state is AMBIGUOUS and automatic re-baseline is prohibited.

### 5.5 Valid v2 plus contradictory supported-corrupt evidence

A valid committed v2 record plus `V2_COMMITTED_INVALID`,
`SUPPORTED_CORRUPT`, or other supported local corruption may represent loss of
a later state.

The valid record's semantic config may be used as the recovery fallback:

```text
semantic_state = UNAMBIGUOUS
token_state    = UNCERTAIN
```

The old token is not accepted.

This class is eligible for the bounded retire-marked fresh-incarnation
re-baseline in §13.

### 5.6 Any supported v2 plus UNSUPPORTED_NEWER

A genuine `ORC1` unknown/newer schema is not automatically erased.

Result:

```text
token_state = UNCERTAIN
automatic re-baseline = prohibited
```

Firmware compatibility or explicit reviewed maintenance is required.

---

## 6. Normal v2 semantic save

No other mutation may own the ConfigStore slot while this decision is made.

For candidate config C':

### 6.1 Semantic no-op

Semantic equality is evaluated before requiring a valid token identity, matching
the approved application CAS ordering.

If:

```text
C' == the recovery-selected UNAMBIGUOUS semantic config
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

- the previously active committed **semantic config** remains the operational
  fallback;
- an incomplete/staged candidate never advances the application token;
- after reboot, token validity is decided by the page classifier rather than by
  assuming every pre-commit failure was clean.

A body write interrupted with an erased commit word can be safely ignored. A
power cut during the inactive-page erase can instead leave
`SUPPORTED_CORRUPT` evidence; in that case the old config remains usable but
token state becomes UNCERTAIN and fresh-incarnation re-baseline may be required.

This availability cost is intentional: a power-cut-damaged inactive page may
invalidate cached remote tokens even though the semantic config survives.

Failure after step 4 but before RAM publication:

- reboot recovery sees the committed successor only when the complete
  generation/incarnation/revision lineage verifies;
- the new config/token then becomes authoritative.

This preserves reset-safe application state without claiming that every failed
physical erase leaves token identity unchanged.

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

## 8. Legacy v1 migration — reviewed contingency, not current implementation

**Non-normative for the current product phase.**

Sections 8 through the legacy-specific parts of §10 document the previously
reviewed migration design in case a future real deployed-fleet requirement makes
migration necessary. They must not be implemented merely because the design
exists.

Current implementation behavior is §1.1: explicit development ConfigStore reset,
then fresh v2 baseline.

### 8.1 Legacy recovery source

v1 has no application token, so local supported-format damage must not be
mistaken for a future schema and permanently wedge migration.

Automatic legacy migration/re-baseline is allowed when:

- no page is `UNSUPPORTED_NEWER`;
- at least one complete semantic config can be selected unambiguously from a
  valid v1 record or a verified staged copy created by this migration family;
- every other non-erased page is supported v1/v2 torn/corrupt evidence that
  does not introduce a different valid semantic config.

For ordinary one/two-valid-v1 recovery, the semantic source is the valid v1
record with the highest generation.

A higher-generation `V1_COMMITTED_INVALID` does **not** become authoritative,
but it also does not permanently block token establishment when a lower valid v1
record supplies the only usable semantic config. This is the supported-corruption
case already permitted by the CAS recovery contract:

```text
operational semantic config = selected valid v1 fallback
token_state                 = UNAVAILABLE/UNCERTAIN until fresh v2 baseline commits
diagnostic                   = recovery degradation recorded
```

The implementation may then migrate that selected fallback with a fresh
incarnation.

If two valid v1 records have equal generation:

- same complete semantic config: semantic state is UNAMBIGUOUS; fresh-incarnation
  migration is allowed with diagnostics;
- different semantic config: semantic state is AMBIGUOUS; automatic migration is
  prohibited and maintenance must select the semantic value.

A genuine `UNSUPPORTED_NEWER` page always blocks automatic rewrite.

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

## 9. Power cut during migration / staged recovery

The recovery invariant is:

> Before every destructive erase, the selected semantic config has at least one
> independently verified persistent copy that is not the page being erased.

CSPRNG generation for the next incarnation also occurs **before** the next
destructive erase. If CSPRNG fails, no erase is started.

No staged token is ever promoted after reboot.

### 9.1 Interrupted target erase or target body write

If a valid legacy source remains and the other page contains
`V1_UNCOMMITTED_OR_TORN`, `V2_UNCOMMITTED_OR_TORN`, or
`SUPPORTED_CORRUPT` caused by the attempted target erase/write, the valid
legacy source remains the semantic authority for migration purposes.

Recovery:

1. records a degradation diagnostic;
2. obtains a fresh incarnation;
3. only after CSPRNG success erases the damaged target;
4. restarts the staged v2 migration.

This directly covers a power cut during the first target-page erase; the damaged
page is **supported local corruption**, not `UNSUPPORTED_NEWER`.

### 9.2 Valid legacy + V2_STAGED_VALID

The staged token is never made VALID and the staged record is not a committed
semantic authority.

The valid committed v1 record remains the migration semantic source even if the
staged semantic config differs. A different uncommitted stage does **not** by
itself make semantic state AMBIGUOUS; it represents an uncommitted attempted
transition and may be discarded.

Recovery:

1. keep the valid legacy page untouched;
2. obtain a fresh incarnation;
3. erase the staged target;
4. write/verify a fresh staged v2 candidate using the selected legacy config;
5. continue normal legacy retirement.

If the stage happened to contain the same config, the result is identical; its
old staged incarnation is still discarded.

### 9.3 Verified staged/partial-commit v2 + supported-corrupt other page

A power cut while erasing the final semantic source or while finalizing a commit
can leave:

```text
V2_STAGED_VALID or V2_PARTIAL_COMMIT
+
SUPPORTED_CORRUPT other page
```

The verified v2 body is now the only unambiguous semantic copy, but its token
still must not be promoted.

Recovery uses its **semantic config only**:

1. keep the verified staged/partial-commit page untouched;
2. obtain a fresh incarnation before any erase;
3. erase the damaged other page;
4. write/verify a fresh staged v2 candidate to that page;
5. erase the old staged/partial-commit page;
6. verify erase;
7. commit the fresh candidate;
8. verify committed record;
9. expose VALID.

The new candidate uses a fresh incarnation. Its storage generation is chosen
strictly above any trusted supported v2 generation observed in this recovery
transaction; generation is ordering metadata, not proof of token continuity.

### 9.4 One staged/partial-commit v2 + one erased page

A staged or partial-commit token is never promoted after reboot, including a
blank-partition/migration/re-baseline interrupted during commit.

When the v2 body/CRC/config/token fields verify, either
`V2_STAGED_VALID` or `V2_PARTIAL_COMMIT` may preserve the semantic config.

Recovery:

1. use that record only as an unambiguous semantic safety copy;
2. obtain a fresh incarnation;
3. write a fresh staged baseline to the erased page;
4. verify;
5. erase the old staged/partial-commit page;
6. commit/verify the fresh candidate;
7. expose VALID.

This prevents a partially programmed final commit word from silently discarding
the user's last verified semantic config while still refusing to trust the
partially committed token.

### 9.5 Two V2_STAGED_VALID pages

Two staged records are never resolved by token/generation alone.

If their complete semantic configs are equal:

```text
semantic_state = UNAMBIGUOUS
token_state    = UNCERTAIN/UNAVAILABLE
```

Recovery obtains a **new** incarnation, keeps one staged page only as the
semantic safety copy, erases the other, writes/verifies a fresh stage there,
then erases the old stage and commits the fresh candidate.

If their semantic configs differ:

```text
semantic_state = AMBIGUOUS
token_state    = UNCERTAIN
```

No automatic erase/re-baseline occurs. Maintenance must select the semantic
config.

### 9.6 After committed v2 verification

Only after commit programming **and readback verification** succeeds may the new
token be published VALID.

Repeated power loss may cause the staged/recovery process to restart with another
fresh incarnation, but it never promotes a previously staged token.

---

## 10. Mixed committed legacy + committed v2 — contingency analysis

**Current implementation rule:** do not auto-reconcile. Report maintenance/reset
required and keep protected config mutation unavailable until the development
ConfigStore partition is explicitly reset.

The remainder of this section is retained only as reviewed contingency analysis
for a future deployed-fleet requirement.

Any partition containing both:

```text
V1_COMMITTED_VALID
and
V2_COMMITTED_VALID
```

starts with:

```text
token_state = UNCERTAIN
```

Physical generations are never compared across schemas and no existing v2 token
from the mixed state is reused.

### 10.1 Same complete semantic config

If v1 and v2 encode the same complete semantic config, automatic recovery is
allowed because semantic state is unambiguous.

The **v2 page must be discarded first**. Erasing the v1 page first is forbidden:
a power cut could leave the old committed v2 page alone and incorrectly make its
old token VALID.

Sequence:

```text
1. keep v1 page as the semantic source
2. obtain fresh incarnation before erase
3. erase the committed v2 page
4. verify erase
5. stage fresh v2 baseline on that erased page
6. verify stage
7. erase the v1 source page
8. verify erase
9. commit/verify the fresh v2 candidate
10. expose VALID
```

This is the same two-page staged/retire/commit family as legacy migration; the old
v2 token identity is destroyed before the last legacy semantic source is erased.

### 10.2 Different semantic configs

If valid v1 and valid v2 configs differ, current flash evidence cannot prove which
semantic value is newer.

Result:

```text
semantic_state = AMBIGUOUS
token_state    = UNCERTAIN
automatic rewrite = prohibited
```

Runtime may use only the separately defined conservative recovery fallback; it
must not present either candidate as authoritative application state.

Explicit authenticated/local maintenance must select the semantic config and then
perform a fresh-incarnation re-baseline.

### 10.3 Invalid/corrupt mixed evidence

If no unambiguous semantic value can be selected, do not auto-rewrite.

If one valid semantic copy remains and the other page is only supported local
corruption, the supported-corruption rules may apply. `UNSUPPORTED_NEWER`
always blocks automatic rewrite.

---

## 11. Unsupported/newer schema

Only the fixed-family discriminator defined in §3.1 creates
`UNSUPPORTED_NEWER`, and only **after** supported/torn-prefix classification:

```text
magic == ORC1
version not in {1, 2, 0xFF}
bytes[5..7] == 0
(version & 0x03) == 0
```

That page is not automatically overwritten merely to regain CAS availability.

Result:

```text
token_state = UNCERTAIN
protected config mutation unavailable
automatic re-baseline prohibited
```

Non-ORC1 torn/corrupt bytes inside this ORUN-owned partition are instead
`SUPPORTED_CORRUPT` evidence and follow the bounded supported-corruption rules.

This contract intentionally requires future ConfigStore schema versions to retain
the fixed ORC1/version discriminator until a separately reviewed classifier
migration says otherwise.

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
state identity cannot remain authoritative and semantic state is
UNAMBIGUOUS.

### 13.1 v2 supported-corruption re-baseline

When a committed v2 page A supplies the selected semantic fallback but token state
is UNCERTAIN because page B contains supported contradictory evidence, the first
implementation uses the page-local retire marker.

The sequence is restart-safe and idempotent at its already-completed destructive
steps:

```text
1. obtain fresh nonzero incarnation before any new erase
2. if A.token_retire_word == FF:
      program it to 0
      read back and verify A is retired
   else:
      do not program it again; treat A as already retired
3. if B is not fully erased:
      erase B
      verify B erased
   else:
      do not erase it again
4. write/verify fresh-incarnation staged v2 baseline on B
5. erase retired semantic-source page A
6. verify A erased
7. commit staged B
8. verify committed B
9. expose token_state = VALID
```

The backend must never attempt to re-program a non-FF retire word, because config
flash programming accepts only erased destination bytes.

If recovery sees a retired A plus an already verified stage/partial-commit on B,
it does **not** resume by promoting B. It follows the recovery-decision table:
the existing records preserve semantics only, a new incarnation is generated,
and a fresh staged candidate is created on an erased page before any token can
become VALID.

If power fails after step 2, A can still supply config but its old token remains
UNCERTAIN forever because the retire word is monotonic.

If power fails after B is erased, the surviving A still carries the retire word,
so §5.1 cannot resurrect its old token.

If power fails after the fresh stage is written, §5.2 also cannot revive A:
A is retired and the fresh stage has a different incarnation.

This closes the token-resurrection path without a third page.

### 13.2 Two committed-valid but impossible-lineage v2 pages

If two committed v2 pages violate normal generation/token lineage:

- same complete semantic config: semantic state is UNAMBIGUOUS; choose one only
  as the semantic safety copy, retire it first, then use the §13.1 sequence;
- different semantic config: semantic state is AMBIGUOUS; no automatic
  re-baseline.

Choosing one same-config page as the physical safety copy does **not** assert that
its token or temporal ordering is authoritative; all existing token identities
are discarded.

### 13.3 Result of successful re-baseline

A successful re-baseline:

```text
preserves the selected semantic config
creates fresh nonzero incarnation
sets revision = 1
uses a storage generation strictly above trusted supported v2 generations when
continuing inside a v2 namespace
publishes VALID only after durable commit/readback
```

Old application tokens become stale.

Re-baseline is diagnosable and is never a generic way to bypass
STALE_PRECONDITION.

A genuine `UNSUPPORTED_NEWER` page or AMBIGUOUS semantic state cannot enter this
automatic sequence.

---

### 13.4 Recovery decision table

The first implementation uses this decision policy:

| observed partition state | runtime config | semantic state | token state | automatic action |
| --- | --- | --- | --- | --- |
| both pages erased | defaults | FALLBACK_ONLY | UNAVAILABLE | establish fresh baseline when safe |
| valid v1 source + erased/torn/supported-corrupt residue, no unsupported schema | selected valid v1 fallback | UNAMBIGUOUS | UNAVAILABLE/UNCERTAIN | fresh-incarnation migration allowed |
| one non-retired committed-valid v2 + erased page | committed v2 config | UNAMBIGUOUS | VALID | none |
| committed v2 + staged exact successor (same incarnation, revision +1) | committed v2 config | UNAMBIGUOUS | VALID | discard/erase stage on later cleanup |
| retired committed v2 + erased other page | retired page config as semantic fallback | UNAMBIGUOUS | UNCERTAIN | skip retire rewrite and erased-page erase; fresh-incarnation re-baseline |
| retired committed v2 + supported-corrupt other page | retired page config as semantic fallback | UNAMBIGUOUS | UNCERTAIN | skip retire rewrite; erase corrupt page, then fresh-incarnation re-baseline |
| retired committed v2 + same-config verified stage/partial-commit | common semantic config | UNAMBIGUOUS | UNCERTAIN | never promote existing stage; fresh-incarnation staged recovery |
| non-retired valid v2 + supported-corrupt contradictory page | valid v2 config as selected fallback | UNAMBIGUOUS | UNCERTAIN | first retire valid token, then re-baseline |
| one verified stage/partial-commit + erased page | staged body config | UNAMBIGUOUS | UNCERTAIN/UNAVAILABLE | §9.4 fresh-incarnation recovery; never promote existing token |
| one verified stage/partial-commit + supported-corrupt page | staged body config | UNAMBIGUOUS | UNCERTAIN | §9.3 fresh-incarnation recovery |
| impossible-lineage v2 records with same semantic config | common config | UNAMBIGUOUS | UNCERTAIN | fresh-incarnation re-baseline allowed |
| two staged v2 records with same semantic config | common config | UNAMBIGUOUS | UNCERTAIN/UNAVAILABLE | fresh-incarnation staged recovery allowed |
| valid v1 + any uncommitted v2 stage, including different config | valid v1 config | UNAMBIGUOUS | UNAVAILABLE/UNCERTAIN | discard stage after fresh CSPRNG; migrate selected v1 config |
| mixed committed v1+v2 with same semantic config | common config; v1 retained as migration source | UNAMBIGUOUS | UNCERTAIN | erase v2 first, then fresh migration |
| committed/otherwise authoritative valid candidates with different semantic configs | safe runtime defaults only | AMBIGUOUS | UNCERTAIN | no automatic rewrite; maintenance selects config |
| only supported corruption, no recoverable semantic copy | defaults | FALLBACK_ONLY | UNCERTAIN/UNAVAILABLE | no equality no-op; maintenance or reviewed recovery |
| any UNSUPPORTED_NEWER evidence | best safe runtime fallback only | AMBIGUOUS | UNCERTAIN | no automatic erase/re-baseline |

The runtime fallback column is an availability policy, not a claim that the
fallback was historically the newest persisted value.

For AMBIGUOUS/FALLBACK_ONLY states, application state reads must expose the
recovery condition; callers must not treat the runtime fallback as a
cache-authoritative config/token pair.

### 13.5 Local maintenance while token is invalid

USB/BLE/local config mutation while token state is UNAVAILABLE or UNCERTAIN is
not a normal save.

If an authorized operator intentionally chooses a semantic config, that action is
an explicit **re-baseline**:

- the selected config becomes the semantic source;
- a fresh CSPRNG incarnation is mandatory;
- revision starts at 1;
- retire/migration ordering for the observed partition state still applies;
- failure to obtain CSPRNG means the semantic change is not durably written.

This prevents a local transport from bypassing the same identity rules enforced
for remote CAS.

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

### 14.3 Mutation ownership and conditional admission

Only one semantic mutation/re-baseline/migration job may own ConfigStore at a
time.

For protected CAS mutation, the following must be one serialized, non-yielding
application-owner decision:

```text
acquire mutation slot
-> read coherent config/token snapshot
-> equality check
-> token-validity / expected-token comparison
-> admit the ConfigStore save job
```

No callback, transport adapter, reset path or probe may mutate config between the
expected-token comparison and save admission.

The implementation may enforce this with a conditional ConfigStore API or with a
single application-owner critical/admission call; the exact C++ spelling is not
frozen here.

### 14.4 Special paths

Migration/re-baseline/reset use explicit paths and acquire the same mutation
ownership.

Test/probe code cannot directly construct a changed semantic record with unchanged
revision.

Current test-only flash probes must use the token-aware API or be disabled for v2.

### 14.5 Unreconciled asynchronous flash mutation

The current FlashMutationGate can report a timeout to a caller after SoftDevice
has already accepted a physical flash operation; the physical SUCCESS/ERROR event
may arrive later.

The v2 config path must expose the same ownership ambiguity signal already
available to security storage:

```text
ConfigPort::hasUnreconciledMutation()
```

or an equivalent reviewed mechanism.

If a config erase/program returns failure while the backend reports an
unreconciled accepted mutation:

- ConfigStore enters a mutation-unreconciled state;
- no new semantic save/reset/migration/re-baseline is admitted;
- the in-RAM token is not exposed as cache-authoritative VALID state;
- a late completion is never interpreted as a second application success;
- once the backend reports reconciliation, ConfigStore performs a **full
  two-page recovery/classification** before accepting another mutation.

A reboot may also perform that recovery. Continuing to write merely because the
original logical request already returned failure is prohibited.

### 14.6 Read snapshot during recovery ambiguity

A state read may return the operational/fallback config and diagnostics while
mutation reconciliation is pending, but it must mark token/recovery validity so
callers cannot cache that pair as authoritative.

---

## 15. Decoder/classifier requirements

The v2 implementation should not replace the current narrow v1 decoder with one
ambiguous "decode anything" function.

Keep explicit versioned parsing.

Suggested logical split:

```text
classify physical evidence
  -> erased
  -> supported-v1
  -> supported-v2
  -> supported-corrupt/torn
  -> unsupported-newer ORC1 version

decode supported v1
decode supported v2 body
evaluate commit + retire state
validate semantic config/token fields
partition-level recovery decision
```

Do not guess an unsupported newer schema's layout, and do not label local
torn-write/erase residue as a future schema merely because its magic/version is
damaged.

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
+12 B v2 record bytes
+ 4 B reserved page-local token-retire word
```

The sealed record is still exactly 48 bytes; the first 52 bytes of each 4096-byte
page are reserved by the v2 ConfigStore layout.

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

### Migration / re-baseline wear

One-time legacy migration may require:

- erase old inactive legacy page;
- one staged v2 body write;
- erase last legacy source page;
- one commit write.

A v2 supported-corruption re-baseline adds one 4-byte retire-word program before
destroying contradictory evidence.

The retire and commit words are each programmed at most once between page erases.
The implementation must respect the reference nRF52 flash datasheet's same-word
programming limit; it must never "toggle" or rewrite either marker in place.

Interrupted migration/re-baseline can cost additional recovery erases/writes, but
recovery must not spin on flash.

### Brownout / repeated-boot wear bound

Automatic destructive migration/re-baseline is not an early-boot retry loop.

The implementation must:

- wait until the normal platform/power policy considers destructive flash work
  admissible;
- allow at most one automatic destructive recovery transaction at a time;
- after a flash/CSPRNG/reconciliation failure, remain
  UNAVAILABLE/UNCERTAIN rather than immediately restarting in the same boot;
- require a later stable retry opportunity or explicit maintenance.

The implementation slice must define the concrete boot/power-stability gate using
existing platform evidence; do not invent a new hardware dependency merely for
this storage migration.

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

The implementation slice must add or adapt host/fault tests for at least the
following families.

### v2 codec / page metadata

1. exact 48-byte golden record encoding;
2. big-endian fields;
3. CRC covers bytes 0..39;
4. commit offset exactly 44;
5. retire-word offset exactly 48;
6. sealed record size remains 48 while page-local reserved footprint is 52;
7. reserved-byte rejection;
8. zero incarnation rejection;
9. zero revision rejection;
10. wrong payload length rejection;
11. unsupported ORC1 version classified as UNSUPPORTED_NEWER;
12. partial/nonzero-non-FF commit rejected as active;
13. any non-FF retire word invalidates token authority;
14. exact v1 bytes remain readable without fixture changes.

### physical classifier / torn-operation model

15. fully erased page;
16. v1 commit FF with incomplete body -> non-authoritative torn/uncommitted;
17. v2 commit FF with incomplete body -> V2_UNCOMMITTED_OR_TORN;
18. exact verified v2 staged record -> V2_STAGED_VALID;
19. programmed-prefix + all-FF tail is not UNSUPPORTED_NEWER;
20. damaged/non-ORC1 local bytes -> SUPPORTED_CORRUPT;
21. ORC1/version unknown -> UNSUPPORTED_NEWER;
22. partial commit word -> SUPPORTED_CORRUPT/UNCERTAIN;
23. partial retire word -> retired/UNCERTAIN;
24. fault injection at every 4-byte body-program boundary;
25. interrupted erase patterns do not become future-schema evidence.

### normal v2 save/recovery

26. blank -> defaults with token initially UNAVAILABLE;
27. successful baseline -> generation 1 / revision 1 / VALID;
28. semantic save increments generation and revision exactly once;
29. unchanged UNAMBIGUOUS config -> no write/no token change even when token
    state is UNAVAILABLE/UNCERTAIN;
30. FALLBACK_ONLY/AMBIGUOUS state cannot claim ALREADY_SATISFIED from defaults;
31. changed config with UNAVAILABLE/UNCERTAIN cannot bypass re-baseline;
32. A -> B -> A produces distinct token revisions;
33. body-stage power cut with clean commit-FF residue preserves old VALID token;
34. inactive-page erase interruption preserves semantic config but makes token
    UNCERTAIN when contradictory supported corruption remains;
35. post-commit/pre-RAM-publication reboot recovers successor only with exact
    generation/incarnation/revision lineage;
36. revision exhaustion fails closed;
37. storage-generation exhaustion fails closed;
38. committed+staged VALID path requires G+1, same incarnation, revision+1;
39. staged different incarnation never leaves old committed token VALID;
40. committed-valid pair requires adjacent generation, same incarnation and
    adjacent revision;
41. equal/non-adjacent/impossible-lineage committed pairs -> UNCERTAIN.

### legacy migration contingency — deferred, not required for current cutover

The following previously reviewed migration tests are **not implementation gates
for the current product phase** because automatic v1 -> v2 migration is not being
implemented. Retain them as contingency requirements only if migration is later
authorized.

42. one valid v1 + erased target -> v2;
43. two valid v1 pages -> higher legacy config preserved;
44. v2 generation begins at 1 independent of v1 generation;
45. CSPRNG failure occurs before destructive migration erase;
46. token never VALID before final legacy retirement + v2 commit verification;
47. power cut during first target erase at every modeled word boundary;
48. power cut during staged body write at every 4-byte boundary;
49. valid v1 + supported-corrupt residue may recover with diagnostics;
50. higher-generation committed-invalid v1 + lower valid v1 uses reviewed safe
    fallback and fresh incarnation, never the invalid record;
51. equal-generation same-config v1 pair may fresh-migrate with diagnostics;
52. equal-generation different-config v1 pair -> AMBIGUOUS/no auto rewrite;
53. UNSUPPORTED_NEWER + valid v1 blocks automatic migration;
54. power cut while final legacy source is erased leaves stage semantic copy
    recoverable but never promotes staged token;
55. one staged + erased page creates a fresh incarnation rather than promoting
    staged incarnation;
56. two same-config staged pages recover through a third/fresh incarnation using
    the same two physical pages;
57. two different-config staged pages -> AMBIGUOUS/no auto erase.

### retire-word / H2 re-baseline

58. supported-corruption v2 re-baseline programs/verifies retire word before
    contradictory page erase;
59. power cut after retire mark but before contradictory erase -> old token stays
    UNCERTAIN;
60. power cut during contradictory erase -> retired survivor cannot regain VALID;
61. power cut after fresh stage -> retired old committed token cannot regain VALID;
62. power cut while erasing retired semantic source -> fresh stage semantics
    survive but staged token is not promoted;
63. commit/readback of fresh candidate is required before VALID publication;
64. same-config impossible-lineage committed v2 pair fresh-rebaselines;
65. different-config impossible-lineage pair -> AMBIGUOUS/no auto rewrite.

### mixed v1/v2 contingency — current runtime must fail closed

For the current clean-cutover product, the required behavior is simpler:

- any committed v1 observed by a v2 runtime => maintenance/reset required;
- any mixed committed v1+v2 => maintenance/reset required;
- no automatic semantic selection or migration.

The detailed cases below remain contingency tests only if migration is later
authorized.

66. mixed committed same-config v1+v2 never reuses v2 token;
67. same-config mixed recovery erases v2 first while v1 remains semantic source;
68. power cut during that v2 erase leaves v1 migration recoverable;
69. mixed different-config v1+v2 -> AMBIGUOUS/no automatic choice;
70. unsupported/corrupt mixed evidence follows decision table and never silently
    resurrects an existing token.

### API / ownership / asynchronous reconciliation

71. normal caller cannot supply revision;
72. semantic mutation cannot retain old revision;
73. expected-token comparison and save admission cannot be interleaved by another
    writer;
74. reset uses same mutation ownership and advances token only for semantic change;
75. test/probe mutation cannot bypass token progression;
76. UNAVAILABLE/UNCERTAIN local write is explicit re-baseline, not normal save;
77. local re-baseline CSPRNG failure does not durably change config;
78. ConfigPort exposes backend unreconciled-mutation state;
79. accepted async erase timeout -> no new config mutation until late event
    reconciles and full partition recovery runs;
80. accepted async body/commit timeout -> no stale RAM token is exposed as
    cache-authoritative;
81. late SUCCESS/ERROR never produces a second application success result;
82. post-reconciliation full recovery determines actual flash state before new
    admission.

### retry/wear bounds

83. automatic recovery never immediately loops after CSPRNG/flash failure in the
    same boot;
84. destructive recovery waits for the implementation's reviewed stable-power
    admission condition;
85. normal unchanged config still causes zero erase/program operations;
86. normal successful semantic save remains one page erase + body/CRC + commit;
87. retire word is programmed at most once before its page is erased.

### final-verify R1-R3 regressions

88. classifier precedence checks exact v1/v2 and recognized torn-prefix evidence
    before UNSUPPORTED_NEWER;
89. half-erased v1 version examples such as 0x03/0x05 are SUPPORTED_CORRUPT or
    recognized torn evidence, never UNSUPPORTED_NEWER;
90. half-erased v2 version examples such as 0x03/0x06/0x0A are
    SUPPORTED_CORRUPT or recognized torn evidence, never UNSUPPORTED_NEWER;
91. ORC1/version 0xFF outside a recognized torn-prefix pattern is
    SUPPORTED_CORRUPT;
92. a genuine future discriminator requires bytes[5..7] == 0 and
    (version & 0x03) == 0; representative version 4 is UNSUPPORTED_NEWER only
    after earlier torn-evidence checks;
93. verified v2 body/CRC/config/token with partial commit word becomes
    V2_PARTIAL_COMMIT: token non-authoritative, semantic config recoverable;
94. V2_PARTIAL_COMMIT + erased page performs fresh-incarnation recovery without
    reverting to defaults;
95. retired committed + erased page does not attempt to program retire word
    again and skips redundant erase;
96. retired committed + same-config stage/partial-commit never promotes the
    existing staged token and fresh-rebaselines;
97. stage/partial-commit + supported-corrupt other page preserves the verified
    semantic copy through fresh-incarnation recovery;
98. valid committed v1 + different-config uncommitted v2 stage keeps v1 as the
    committed semantic source and may discard the stage;
99. recovery restart never submits a program operation to a non-FF retire word.


---

## 19. Physical validation boundary

Host fault tests can establish software state-machine behavior against a flash
model.

They do not prove real nRF52840 power-cut behavior.

The implementation closure must separately decide the smallest physical test that
can validate:

- staged write -> power cut;
- legacy retirement -> power cut;
- token-retire-word program/readback;
- commit-last recovery;
- BLE/SoftDevice flash-gate coexistence and late-completion reconciliation if the
  runtime path uses it.

A weak/partially programmed flash word can be a physical phenomenon not fully
modeled by host byte arrays. Recovery therefore treats any non-FF retire word as
retired and any non-FF/non-zero commit word as ambiguous.

The implementation must also respect the pinned nRF52840 documentation's
same-word programming limit. No exact physical endurance/power-cut claim is made
by this design document.

Do not report those behaviors as physically validated until performed on actual
hardware.

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
