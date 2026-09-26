# ConfigStore v2 Tokenized Layout Audit Disposition

Status: **FOCUSED INDEPENDENT AUDIT — PASS WITH FIXES; FINAL VERIFY PASS WITH MINOR DOC FIX; R1-R3 APPLIED — DOCUMENTATION-ONLY.**

Baseline:

`main@3c79b47e5e56f84caab5e0d4bdcb7a4397925e13`

Audit target:

`PR #44 / design/config-store-v2-tokenized-layout@6715929d1e615baaaa2ace0c472e0bc3b7b5daae`

Primary design source:

`docs/architecture/ORUN_CONFIG_STORE_V2_TOKENIZED_LAYOUT.md`

This file records durable audit conclusions and accepted corrective actions only.
Raw reviewer prompts/transcripts are not architecture sources.

---

## 1. Verdict

Independent focused audit verdict:

**PASS WITH FIXES**

Severity summary:

- BLOCKER: 0
- HIGH: 2
- MEDIUM: 4
- LOW / DOC: 5

The reviewer accepted the architectural foundation:

- exact 48-byte ConfigStore v2 sealed record;
- existing two 4096-byte ConfigStore pages are sufficient;
- no third page is required;
- body/CRC stage -> legacy retirement -> commit is a valid migration family;
- 64-bit incarnation + 32-bit revision remains the approved application token;
- v1/v2 physical generations remain separate schema namespaces;
- normal save wear remains one erase + body/CRC program + commit program;
- no TLP v1/BLE/RF/runtime change is made by this documentation slice.

The review found two implementation-blocking recovery-state gaps, not a need for
architectural redesign.

---

## 2. HIGH finding disposition

### H1 — torn erase/write residue was misclassified as unsupported schema

**Accepted.**

Problem:

Ordinary power loss during page erase or record programming can leave a page
whose magic/version/body is incomplete while old commit bits may remain. Treating
every non-decodable page as an unknown/newer schema can permanently wedge
migration/recovery after a routine power cut.

Applied correction:

- `UNSUPPORTED_NEWER` now requires the fixed ORC1 family discriminator with an
  unknown version;
- future ConfigStore schemas must preserve the ORC1/version discriminator until
  a separately reviewed classifier migration;
- exact supported v1/v2 commit offsets are evaluated before full decode;
- commit-FF supported records are non-authoritative torn/uncommitted evidence;
- damaged/non-ORC1 bytes inside the exclusively owned ConfigStore partition are
  `SUPPORTED_CORRUPT`, not automatically future schema;
- programmed-prefix + all-FF tail is recognized as local torn-write evidence;
- supported corruption may use bounded fresh-incarnation recovery when one
  semantic fallback is unambiguous;
- genuine unsupported/newer ORC1 versions are never auto-overwritten.

Legacy migration is therefore not permanently blocked merely because its own
target erase/write was interrupted.

### H2 — re-baseline could erase contradictory evidence and resurrect old token

**Accepted.**

Problem:

Starting from an UNCERTAIN v2 state, erasing the contradictory page can leave a
single old committed v2 page. Without durable evidence that its token had already
been invalidated, reboot recovery could classify that old token as VALID again.

Applied correction:

A page-local monotonic `token_retire_word` is reserved at page offset 48.

- sealed v2 record remains exactly 48 bytes;
- retire word is outside record CRC;
- `0xFFFFFFFF` means not explicitly retired;
- any non-FF value means token retired/non-authoritative;
- implementation writes `0x00000000`;
- retire write must be read back before contradictory evidence is erased;
- a retired page may preserve semantic config but can never make its old token
  VALID;
- retire word is programmed at most once before page erase.

Supported-corruption v2 re-baseline order is now:

```text
fresh CSPRNG
-> program/verify survivor retire word
-> erase contradictory page
-> stage/verify fresh-incarnation candidate
-> erase retired semantic-source page
-> commit/verify fresh candidate
-> expose VALID
```

The normal committed+staged VALID case additionally requires:

- storage generation +1;
- same incarnation;
- revision +1;
- committed page not retired.

A different-incarnation staged record therefore cannot make an older committed
token VALID.

---

## 3. MEDIUM finding disposition

### M1 — migration/staged recovery and two-stage state were underspecified

**Accepted.**

Applied invariant:

> Before every destructive erase, the selected semantic config has at least one
> independently verified persistent copy that is not the page being erased.

Additional rules:

- CSPRNG is obtained before each destructive recovery phase;
- staged token identity is never promoted after reboot;
- one staged + erased recovery creates a fresh incarnation;
- two staged records with the same semantic config may fresh-rebaseline;
- two staged records with different semantic configs are AMBIGUOUS and are not
  automatically erased/reconciled.

### M2 — same-config mixed v1+v2 erase order could resurrect v2 token

**Accepted.**

For mixed committed v1+v2 with identical semantic config:

- keep v1 as semantic source;
- obtain fresh incarnation;
- erase the committed v2 page **first**;
- stage fresh v2;
- erase v1 only after the fresh stage verifies;
- commit/verify fresh v2;
- no existing mixed-state v2 token is reused.

If v1/v2 semantic configs differ, the state remains AMBIGUOUS/UNCERTAIN and
requires explicit maintenance selection.

### M3 — UNCERTAIN states lacked an operational/recovery decision table

**Accepted.**

The design now derives both:

```text
token_state = VALID / UNAVAILABLE / UNCERTAIN
semantic_state = UNAMBIGUOUS / FALLBACK_ONLY / AMBIGUOUS
```

A decision table defines:

- runtime config fallback;
- semantic confidence;
- whether equality/no-op is allowed;
- whether automatic migration/re-baseline is permitted;
- when maintenance is required.

Equality/no-op under invalid token state is allowed only when semantic state is
UNAMBIGUOUS.

### M4 — pre-commit failure wording overstated token continuity

**Accepted.**

A failed/torn normal save preserves the previous semantic config, but it does not
guarantee the previous token remains VALID after reboot.

In particular, interrupted inactive-page erase can leave supported-corrupt
evidence. The semantic config remains usable, while token state becomes UNCERTAIN
and may require fresh-incarnation re-baseline.

This availability cost is explicit and must be fault-tested.

---

## 4. LOW / DOC finding disposition

### L1 — weak/partial flash-word behavior

**Accepted.**

- any non-FF retire word fails safe as retired;
- non-FF/non-zero commit is ambiguous;
- commit/retire words are each programmed at most once between erases;
- implementation must respect the pinned nRF52840 same-word programming limit;
- host models do not prove weak-write physical behavior, so focused hardware
  validation remains required.

### L2 — asynchronous config mutation can time out then physically complete

**Accepted.**

Current FlashMutationGate already preserves an accepted timed-out physical request
until a late completion event, but ConfigPort does not currently expose the
unreconciled state.

The v2 implementation must add equivalent config visibility through
`hasUnreconciledMutation()` or an equivalent reviewed mechanism.

While a config mutation is unreconciled:

- no new config mutation/re-baseline is admitted;
- in-RAM token is not cache-authoritative;
- late completion does not become a second application success;
- after reconciliation clears, a full two-page recovery/classification is
  required before new mutation.

### L3 — expected-token check and save admission must be atomic at owner level

**Accepted.**

The application/config owner must serialize:

```text
slot acquisition
-> coherent state read
-> equality
-> token validity / expected-token comparison
-> save admission
```

without yielding to another config writer.

Reset/probe/special paths use the same mutation ownership.

### L4 — local writes while token invalid need explicit semantics

**Accepted.**

USB/BLE/local semantic change while token is UNAVAILABLE/UNCERTAIN is not a
normal save. An authorized operator-selected change is an explicit re-baseline
requiring a fresh CSPRNG incarnation.

CSPRNG failure means the semantic change is not durably written.

### L5 — brownout/reboot recovery can create wear loops

**Accepted.**

Automatic destructive migration/re-baseline:

- is not an early-boot spin loop;
- waits for the reviewed platform/power flash-admission condition;
- performs at most one destructive recovery transaction at a time;
- does not immediately restart after flash/CSPRNG/reconciliation failure in the
  same boot;
- requires a later stable retry opportunity or explicit maintenance.

The implementation slice must define the concrete gate using existing platform
evidence rather than adding speculative hardware.

---

## 5. Required implementation evidence

The design now requires host/fault coverage for:

- exact 48-byte v2 codec and page-local retire metadata;
- every relevant 4-byte program interruption boundary;
- interrupted page erase classifications;
- staged/retired/mixed v1-v2 states;
- impossible v2 generation/token lineage;
- all retire-word re-baseline power-cut points;
- two-stage recovery;
- async flash timeout / late-completion reconciliation;
- mutation-owner atomic admission;
- CSPRNG-before-erase;
- brownout/retry wear bounds.

The architecture document contains the normative detailed matrix.

---

## 6. Final focused verification

Final focused verification compared the original audited head:

`6715929d1e615baaaa2ace0c472e0bc3b7b5daae`

with the applied-fix head:

`081a0bff1143b0900bc819599c340c9d27f7add9`

and returned:

**PASS WITH MINOR DOC FIX**

with:

- BLOCKER: 0
- HIGH: 0
- H1/H2: closed, subject only to final low/doc clarifications R1-R3;
- M1-M4: closed;
- L1-L5: closed;
- 48-byte sealed record: accepted;
- page-local token-retire word: accepted;
- two ConfigStore pages: sufficient;
- third page: not required;
- stage -> retire -> commit family: accepted;
- async flash reconciliation direction: accepted;
- flash-wear direction: accepted.

The final verification explicitly walked power-cut points and found no path that
makes an old token VALID again.

### R1 — torn version byte could still resemble a future schema

**LOW / DOC — applied.**

The classifier now has a normative precedence:

```text
erased
-> exact supported v1/v2
-> recognized torn/prefix evidence
-> UNSUPPORTED_NEWER discriminator
-> SUPPORTED_CORRUPT
```

A genuine future ORC1 schema additionally requires:

```text
bytes[5..7] == 0
(version & 0x03) == 0
```

so half-erased v1/v2 version values such as 3/5/6/10 cannot masquerade as a
future schema. Future schema versions must use the reserved multiple-of-four
namespace unless a separately reviewed classifier migration changes this rule.

`ORC1/version=0xFF` outside a recognized torn prefix is
`SUPPORTED_CORRUPT`, not `UNSUPPORTED_NEWER`.

### R2 — partial final commit could lose the only verified semantic config

**LOW / DOC — applied.**

The design adds `V2_PARTIAL_COMMIT`:

- full v2 body/CRC/config/token fields verify;
- commit word is neither erased nor fully committed;
- token identity is never authoritative;
- verified semantic config remains usable as a recovery safety copy.

Recovery treats it like a staged semantic copy and creates a fresh incarnation
before CAS resumes. This prevents a partial final commit from silently reverting
the runtime semantic config to defaults.

### R3 — recovery table gaps and retire-word retry trap

**LOW / DOC — applied.**

Recovery/re-baseline is now restart-safe:

- an already non-FF retire word is never programmed again;
- an already erased target page is not redundantly erased;
- retired committed + stage/partial-commit has an explicit fresh-incarnation
  recovery path;
- stage/partial-commit + erased and + supported-corrupt states are explicit;
- valid committed v1 remains the semantic source even when an uncommitted v2
  stage contains a different config; the stage may be discarded because it was
  never committed;
- no recovery path promotes an old staged token.

Regression tests for R1-R3 are added to the required host fault matrix.

No further broad architecture audit is required before merge; only review of the
final documentation diff is needed.

---

## 6. Evidence boundary

This disposition is documentation/architecture evidence only.

It does **not** claim:

- ConfigStore v2 firmware implemented;
- host tests PASS;
- sanitizer/warnings PASS;
- RAK4630 production build PASS;
- physical power-cut PASS;
- weak-write behavior physically validated;
- BLE/SoftDevice async config reconciliation physically validated;
- RF/power validation;
- TLP v1/BLE GET_CONFIG changes;
- COMMAND/RESULT wire freeze.

---

## 7. Closure

The independent audit corrections H1/H2, M1-M4 and L1-L5 are applied.

Final focused verification returned **PASS WITH MINOR DOC FIX** with 0 BLOCKER /
0 HIGH. The residual R1-R3 documentation fixes are also applied.

This closes the ConfigStore v2 **design/audit gate only**.

### Owner scope correction after audit

ORUN currently has no deployed ConfigStore fleet whose v1 development data must
survive a product upgrade. The owner therefore narrowed the current
implementation target after this audit:

```text
development v1 ConfigStore
-> explicit ConfigStore partition erase
-> fresh v2 baseline
-> v2-only normal writes
```

Automatic v1 -> v2 migration and automatic mixed v1/v2 reconciliation are **not**
current implementation requirements. The audit's legacy-migration analysis is
retained as contingency evidence only if a real deployed-fleet requirement is
approved later.

This scope correction simplifies the implementation and does not change the
audited 48-byte v2 record, token-retire word, v2 A/B power-cut recovery, partial
commit salvage, async mutation reconciliation or TLP v1 wire compatibility.

Implementation must still produce the host/fault, compiler/sanitizer, RAK4630
build and focused physical power-cut/SoftDevice evidence required by the
architecture document. No runtime validation is inherited from this disposition.
