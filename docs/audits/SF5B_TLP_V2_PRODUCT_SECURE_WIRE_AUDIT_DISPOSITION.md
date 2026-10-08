# SF5B TLP v2 Product Secure Wire — Independent Audit Disposition

Status: **FINAL FOCUSED RE-REVIEW PASS WITH MINOR DOC FIX — 0 BLOCKER / 0 HIGH /
0 MEDIUM. H1-H2 / M1-M3 / L1 remain CLOSED; follow-up N1-N3 documentation
findings are CLOSED. FINAL RECOMMENDATION: MERGE.**

Audit target:
`pr82@23999959e9cc9f79e1a5da4b5c0df527eb356247`

Base:
`main@cdb9db172d178acc877b9315453e7adbd7947985`

Fixes applied through:
`32a2a320d4afb9c4a9fd63cc36eade67d8df5b90`
before this disposition record was added.

Initial verdict:

```text
VERDICT: PASS WITH FIXES
BLOCKER: 0
HIGH:    2
MEDIUM:  3
LOW:     1
FINAL RECOMMENDATION: FIX THEN RE-REVIEW
```

The review was a documentation/repository consistency and protocol/security
architecture audit. It did not run production code, RF tests, physical storage
tests or regulatory qualification.

## H1 — Tracker custody ACK acceptance / revocation authority

Severity: **HIGH**

Finding:
The candidate ACK KDF/wire contained policy-floor and generation fields but did
not define what durable tracker state made those values acceptable. Reusing the
existing four delegated command slots would incorrectly merge custody and
command authority.

Disposition: **FIXED**

Applied contract:

- tracker owns a separate durable custody authority domain:
  `custody_policy_floor` plus bounded
  `(gateway_device_id, custody_grant_generation)` slots;
- initial product bound is four custody-capable gateways per tracker, separate
  from four command-capable delegated slots;
- ACK floor must exactly equal the durable custody floor;
- ACK generation must exactly equal the generation in the matching custody slot;
- lower floor/generation is stale and rejected;
- higher floor/generation is also rejected until an authenticated policy update
  is durably committed;
- an ACK never advances policy or enrolls/re-enrolls a Gateway;
- slot removal/generation replacement/floor advance becomes authoritative only
  after durable security-owner commit;
- production custody is blocked until this separate tracker-side state exists;
- ACK field/KDF names now explicitly use `custody_policy_floor` and
  `custody_grant_generation` to avoid accidental command-plane coupling.

The exact SecurityStore-v3 bytes and policy-update transport remain later
implementation slices.

## H2 — Per-record release airtime / sender policy

Severity: **HIGH**

Finding:
Every retained durable record needs a durable release fact under the current
exact-object ACK design, so pacing ACKs cannot make the long-term per-record
airtime disappear. The original text could be read as normal live publication
plus later custody, which increases rather than reduces airtime.

Disposition: **FIXED**

Applied contract:

- normal durable PERIODIC/EVENT transmission uses one
  `CUSTODY_REQUESTED=1` protected object;
- that same object may be forwarded live while Gateway durable commit occurs;
- one successful custody transfer is required before tracker responsibility is
  released, unless a later separately reviewed aggregate/equivalent release
  protocol is added;
- `CUSTODY_REQUESTED=0` is a bounded best-effort exception, not the routine
  durable path;
- an exceptional critical live EVENT remains additive airtime and does not
  release its durable record;
- capacity tables now show both:
  1. normal single custody-requested frame + ACK;
  2. separate live + later custody + ACK;
- the text explicitly states that pacing helps collision timing but not
  long-term per-record airtime;
- a future batch/selective aggregate release is left as a separate reviewed
  protocol change, not silently assumed.

The corrected raw reference figures include approximately:

```text
normal PERIODIC + ACK        ~= 3.613 s/record
live + custody + ACK         ~= 5.829 s/record
```

at the current illustrative SF11/BW125/CR4/5 profile.

## M1 — Exact custody object lifetime

Severity: **MEDIUM**

Finding:
The initial text allowed interpretation that a custody object could be sent
before exact bytes became durable, and did not forbid re-protection merely
because a retry/attempt timer rolled over. RESULT custody participation in the
four-object bound was also unclear.

Disposition: **FIXED**

Applied contract:

- custody-requested exact bytes (or audited exact-byte-equivalent state) become
  durable **before first RF transmission**;
- if exact bytes cannot become durable, the custody attempt is not transmitted;
- retry/backoff/attempt timeout never justifies re-protection;
- one active custody attempt retransmits byte-identically;
- re-protection is limited to exact-cache loss/corruption, credential-lifetime
  change, or another separately reviewed security-invalidating condition;
- every replacement uses a fresh security counter and bounded diagnostics;
- the initial maximum four outstanding exact custody objects applies across
  PRODUCT_SECURE PERIODIC/EVENT **and custody-eligible DELEGATED_D2GW RESULT**.

## M2 — command_id reuse with a different payload

Severity: **MEDIUM**

Finding:
The original RESULT retry wording did not define how the tracker decides that a
retry is the same logical CONFIG_SET_DESIRED request. Reusing only
`command_id` could return an older command's outcome for a different desired
state.

Disposition: **FIXED**

Applied contract:

- CONFIG_SET_DESIRED retry always evaluates through normative CAS §6;
- `command_id` alone never authorizes RESULT reuse;
- canonical retained logical-request identity is:

```text
(opcode,
 command_id,
 expected_state_token[12],
 tracking_interval_seconds,
 battery_capacity_mah)
```

- a matching retained tuple suppresses duplicate durable RESULT persistence;
- emitted result status still follows CAS §6 for the current attempt (for
  example prior APPLIED may become ALREADY_SATISFIED);
- same command_id with a different canonical tuple fails closed as
  `POLICY_REJECTED / COMMAND_ID_REUSE_CONFLICT`;
- only bounded retained-result metadata is required; no generic persistent
  command-ID journal is introduced.

## M3 — PERIODIC canonical encoding

Severity: **MEDIUM**

Finding:
The original exact-byte candidate had contradictory ACTIVITY validity/coverage
text and did not fully specify canonical bytes for invalid location/age states.

Disposition: **FIXED**

Applied contract:

- `period_duration_seconds > 0`;
- PERIOD time valid/invalid zero rules are explicit;
- missing location has one canonical byte representation;
- stale altitude/GNSS/age values are forbidden when the relevant validity is
  clear;
- `LOCATION_AGE_VALID=0` requires `0xFFFF`;
- `LOCATION_AGE_VALID=1` requires value `<=0xFFFE`;
- non-GNSS source requires HDOP/satellite zero;
- ACTIVITY schema relationship is now strict:
  - coverage 0 -> UNKNOWN / invalid;
  - partial coverage -> PARTIAL / valid;
  - full coverage -> COMPLETE / valid;
- period zero can no longer satisfy both UNKNOWN and COMPLETE;
- activity invalid forces activity fields to canonical zero;
- transition/intensity saturation values are explicit.

These are candidate schema-v1 canonical bytes for later golden/malformed tests.

## L1 — Airtime value / header bounds

Severity: **LOW**

Finding:
The 72-byte relay-wrapped custody ACK airtime was copied from the 73-byte value,
and PRODUCT_SECURE public header epoch/counter reject rules were too generic.

Disposition: **FIXED**

Applied contract:

- 72-byte calculated airtime corrected to approximately **1642.496 ms**;
- PRODUCT_SECURE explicitly rejects:
  - `key_epoch == UINT32_MAX`;
  - `security_counter == 0`;
- custody ACK candidate header also records explicit invalid floor/generation
  bounds;
- visible header values do not self-advance security state.

## Independently verified invariants retained

The audit also verified and the fix pass preserves:

- no v1/v2 type collision under version+type dispatch;
- frozen SF2/SF3 HISTORY_SECURE bytes are not widened/reinterpreted;
- PRODUCT_SECURE AAD authenticates type/profile/path/family/header;
- root D2A nonce construction retains one shared SecurityStore TX counter
  namespace and no same-key/same-nonce path was found;
- exact retry is byte-identical and changed protected content consumes a fresh
  counter;
- profile 0x01 is not exported as an offline phone read key;
- custody ACK uses domain-separated HKDF + SHA-256 + HMAC-SHA256, full object
  digest binding and idempotent ACK replay;
- logical product identity remains distinct from transport security counter;
- relay metadata is untrusted and exact inner protected bytes are preserved;
- delegated CONFIG_SET_DESIRED and CONFIG_STATE_READ fit the frozen 32-byte
  delegated plaintext ceiling;
- RESULT keeps CAS/OUTCOME_UNKNOWN semantics;
- custody is not APPLIED;
- current airtime/load numbers are calculations, not RF or regulatory evidence.

## Deferred but still required before production

The initial audit accepted these as later gates:

- pre-storage opaque-frame DoS/admission decision;
- read-capable offline profile cryptography/lifecycle;
- battery thresholds;
- subsystem ID registry;
- SF5C exact-cache/selective-release/open-EVENT/RESULT persistence bytes;
- versioned Gateway CustodyStore geometry for the actual SF5 object size;
- host/RAK KDF/HMAC/KAT vectors and crypto coexistence proof;
- regional RF duty-cycle/regulatory analysis.

This disposition additionally makes separate tracker custody-authority
persistence an explicit production blocker until implemented.

## Evidence boundary

This disposition records documentation design corrections only.

It does **not** prove:

- runtime codec/crypto correctness;
- SecurityStore custody-authority persistence;
- Gateway custody runtime;
- real RF airtime/collision/capacity;
- regional regulatory compliance;
- physical flash/power-cut/wear behavior;
- power consumption;
- offline phone decryption;
- Tracker -> Gateway -> Edge -> Backend end-to-end behavior.

## Post-audit owner clarification — connected Gateway write-around

After the initial audit/fix pass, the owner explicitly froze the intended
Gateway connected-path behavior:

```text
verified authenticated exact-object Edge durable accept before fallback
  -> no Gateway flash write
  -> Gateway may ACK tracker

no verified durable Edge accept / timeout / failure / uncertainty
  -> Gateway local durable commit/readback
  -> then Gateway may ACK tracker
```

Connectivity alone is not durability. Once local flash fallback begins, a late
Edge durable-accept does not cancel the in-flight flash mutation; the Gateway
must reconcile that mutation to a known state first.

This clarification changes no PRODUCT_SECURE/GATEWAY_CUSTODY_ACK bytes, but it
does refine custody ownership/concurrency behavior and Gateway flash-wear
policy. The older SF4 local-commit-only contract is now explicitly retained as
historical HISTORY_SECURE behavior and superseded only for the new SF5 product
path.

The write-around branch is also explicitly **production-disabled until a
separately reviewed authenticated exact-object EDGE_DURABLE_ACCEPT contract
exists**. Until then, Gateway runtime must use the reviewed local durable
custody path before ACK.

Therefore these clarifications remain inside the required focused independent
re-review scope.

## Earlier focused re-review at fb958a5a — historical result

A focused re-review of the earlier head `fb958a5a` returned:

```text
VERDICT: PASS WITH MINOR DOC FIX
BLOCKER: 0
HIGH:    0
MEDIUM:  0
LOW:     2
FINAL RECOMMENDATION: MERGE
```

All previous H1/H2/M1/M2/M3/L1 findings were CLOSED.

The two LOW documentation notes from that review are now also closed on the
current branch:

- N1: EVENT uses the same canonical location-age sentinel rules as PERIODIC
  (`0xFFFF` unknown; AGE_VALID requires <= `0xFFFE`);
- N2: same-`command_id` / different canonical request tuple is rejected while
  the serialized Config/CAS owner is held **before any ConfigStore mutation or
  token/state write**.

That review is **not the final review for the current PR head**, because later
owner decisions added the 128 KiB storage supersession, SF5 Gateway Edge-durable
write-around policy, and explicit SF4->SF5 custody-contract supersession.

## Final focused re-review at 4fbd6da7

The independent focused review of `4fbd6da7826891643b6dead9512565d7564baa8c`
returned:

```text
VERDICT: PASS WITH MINOR DOC FIX
BLOCKER: 0
HIGH:    0
MEDIUM:  0
LOW:     3
FINAL RECOMMENDATION: MERGE
```

It revalidated the full PR against base and confirmed:

- H1/H2/M1/M2/M3/L1 remain CLOSED;
- the 128 KiB supersession arithmetic and evidence boundary are correct;
- SF5 Gateway write-around is correctly gated and does not conflict with
  historical SF4 HISTORY_SECURE custody semantics;
- EDGE_DURABLE_ACCEPT is not assumed implemented and remains a production gate;
- TLP v1, frozen SF2/SF3 HISTORY_SECURE and frozen DELEGATED_SECURE_APP bytes
  remain unchanged;
- no runtime/physical/RF/regulatory PASS is claimed.

The three LOW documentation items are closed on the final branch:

- N1: EVENT location-age canonical encoding now matches PERIODIC;
- N2: COMMAND_ID_REUSE_CONFLICT is checked under serialized Config/CAS ownership
  before any ConfigStore mutation/token write;
- N3: SF5G now requires measured Edge-wait + worst-case local fallback
  commit/readback + custody-ACK airtime to fit the tracker ACK rendezvous
  window; otherwise the Gateway skips Edge waiting and goes directly to local
  durable custody for that attempt.

No further independent re-review was required by the reviewer for N1-N3.

Final disposition: **MERGE APPROVED**.
