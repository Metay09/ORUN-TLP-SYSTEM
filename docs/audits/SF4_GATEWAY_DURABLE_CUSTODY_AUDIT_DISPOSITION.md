# SF4 Gateway Durable Custody — Independent Audit Disposition

Status: **FINAL INDEPENDENT CONTROL PASS AT ae00e43a5eb2b7c467c1e81b39e6d2d225960493; 0 BLOCKER / 0 HIGH / 0 MEDIUM / 0 LOW. SF4A DOCUMENTATION CONTRACT CLOSED.**

PR: #79

Branch: `design/sf4-gateway-durable-custody-contract`

Canonical design:
`docs/architecture/ORUN_GATEWAY_DURABLE_CUSTODY.md`

Related contracts:

- `docs/architecture/ORUN_HISTORY_STORE_FORWARD_DELIVERY.md`
- `docs/architecture/ADR_M7P6_SECURITY_ARCHITECTURE.md`
- `docs/architecture/ORUN_TLP_V2_DELEGATED_COMMAND_SECURITY_DIRECTION.md`

This disposition retains the actionable independent-review result without
keeping the raw reviewer conversation as a canonical architecture source.

The independent summary did not print an exact audited head SHA. The review was
performed against PR #79 before the fixes recorded below. Do not apply its FAIL
or any future PASS retroactively to later unrelated runtime/wire changes.

---

## 1. Initial verdict

Independent review verdict:

```text
FAIL
BLOCKER 1
HIGH    4
MEDIUM  6
LOW     5
```

The reviewer accepted the high-level responsibility chain and opaque-custody
direction, but found one data-loss blocker plus authority, airtime, persistence
and consistency gaps that made the contract unsafe to implement as written.

No firmware build, host test or physical hardware test was part of this
documentation audit.

---

## 2. BLOCKER disposition

### B1 — custody delay conflicted with backend D2A replay/epoch policy

Risk:

A tracker could release its only local responsibility after gateway custody,
while the opaque object remained offline long enough that the backend's generic
bounded D2A replay window or current-epoch-only rule later rejected it. Newer
frames arriving through another gateway could therefore make the old custodied
object permanently unacceptable, causing real data loss.

Disposition applied:

- HISTORY_SECURE immutable observation ingest now has an explicit
  family-specific exception to the generic M7P6 backend D2A replay rule;
- after successful AEAD authentication, an immutable History observation is not
  rejected solely because its security counter is older than the bounded
  reordering window;
- backend dedupe remains by stable logical History observation identity;
- the exception is explicitly forbidden for COMMAND/RESULT/config/actuation and
  other side-effecting families;
- backend/security authority retains retired D2A credential/epoch material as
  decrypt-only while authorized Gateway/Edge custody from that lifetime remains
  unresolved;
- the generic M7P6 security ADR now cross-references this narrow SF4 exception.

Status: **FIX APPLIED; FOCUSED RE-VERIFICATION REQUIRED.**

---

## 3. HIGH dispositions

### H1 — Gateway -> Edge and Edge -> Backend release evidence was unauthenticated

Risk:

After tracker release, an unauthenticated local client or false network success
could cause the remaining custody copy to be reclaimed.

Disposition applied:

- Gateway reclaim requires authenticated object-bound durable acceptance from an
  enrolled/authorized Edge;
- volatile BLE/USB/UART transfer or unauthenticated local response is
  insufficient;
- Edge reclaim requires authenticated backend durable acceptance, such as
  server-authenticated TLS plus an application result bound to the submitted
  object, or an application-layer authenticated receipt;
- handoff evidence must survive reset/recovery.

Status: **FIX APPLIED; FOCUSED RE-VERIFICATION REQUIRED.**

### H2 — custody scope, airtime and flash-wear budgets were undefined

Risk:

The existing one-probe/hour SF3 policy cannot drain a tracker producing multiple
records/hour, while ACK-per-record at SF11/BW125 can become expensive at
30-50/~100 nodes and may create unacceptable gateway TX occupancy and internal
flash wear.

Disposition applied:

- first SF4 custody runtime is explicitly limited to HISTORY_SECURE backlog;
  legacy live TLP v1 POSITION remains unchanged;
- when authenticated custody contact is present, SF4D must use a separate bounded
  connected-drain mode;
- confirmed custody drain rate must exceed retained-record production rate with
  documented engineering margin for each claimed cadence/topology;
- SF4C/D must quantify gateway half-duplex ACK airtime, RX rendezvous,
  retry/collision load and regional channel/duty constraints;
- SF4B must provide a quantitative internal-flash write/erase/wear model including
  metadata, reclaim, reboot duplicates and queue churn;
- bounded multi-object/delayed ACK, Edge-connected fast handoff and RF
  profile/domain planning are listed only as candidate mechanisms, not frozen
  solutions.

Status: **FIX APPLIED; FOCUSED RE-VERIFICATION REQUIRED.**

### H3 — compromised/failed authorized gateway trust was implicit

Risk:

An enrolled gateway able to produce valid custody ACKs can falsely acknowledge
and discard data. This is a fundamental custody trust boundary.

Disposition applied:

- an enrolled custody gateway is explicitly documented as a data-loss trust
  anchor during its valid authority lifetime;
- custody ACK authority must be bound at least per
  `(tracker, gateway_device_id, policy_floor, grant_generation)`;
- shared site/fleet/group custody-authentication keys are prohibited;
- revocation stops future custody-ACK acceptance after the tracker learns the
  policy change;
- revocation does not invalidate opaque already-held HISTORY_SECURE objects;
  they may still flow downstream for backend authentication/dedupe.

Status: **FIX APPLIED; FOCUSED RE-VERIFICATION REQUIRED.**

### H4 — SoftDevice-era durable commit completion was undefined

Risk:

Submitting a flash write is not equivalent to a durable commit under
SoftDevice-enabled asynchronous flash. ACK-before-commit could occur, and a
page erase on the ACK critical path could miss the tracker's RX window.

Disposition applied:

- durable commit now requires successful flash completion event/result plus
  readback/integrity verification and authoritative commit seal/marker;
- late/error/timeout/unknown result produces no custody ACK;
- the first queue design must maintain bounded pre-erased/prepared admission
  reserve so ordinary ACK admission does not require page erase in the tracker
  receive-window critical path;
- worst-case flash completion/readback/ACK latency is a required rendezvous
  input.

Status: **FIX APPLIED; FOCUSED RE-VERIFICATION REQUIRED.**

---

## 4. MEDIUM dispositions

### M1 — opaque ACK language was inconsistent

Fix:

Gateway custody language now consistently binds ACK to the exact opaque protected
custody object. Only tracker/backend owners that can map/decrypt may speak in
logical HistoryRecordIdentity terms.

### M2 — validation wording weakened the no-eviction invariant

Fix:

Normal queue/capacity pressure **cannot** evict ACKed custody. Catastrophic
storage failure is a separately diagnosed fault, not a permitted eviction path.

### M3 — forged opaque frames could consume durable capacity

Fix:

SF4B/C must evaluate a custody-specific outer admission authenticator verifiable
by the gateway without `K_root`. Until such a path is reviewed, targeted
claimed-source quota exhaustion remains an explicit non-claim. Rate/capacity/wear
bounds still apply and forged traffic cannot evict ACKed custody.

### M4 — retry reset could create repeated opaque duplicates

Fix:

While the protected frame survives in tracker RAM, ordinary retry/backoff cycles
reuse it byte-identically. Re-protection is reserved for reboot/lost RAM object,
credential-lifetime change or another explicit security-invalidating condition.

### M5 — fingerprint-only dedupe was unsafe

Fix:

Fingerprint equality is only an index/ACK-binding aid. Gateway duplicate
suppression confirms full object length + byte equality. Every custody handoff
re-verifies full-object fingerprint/length for corruption/substitution defense.

### M6 — custody ACK authentication lifecycle was underspecified

Fix:

SF4C now prefers an idempotent object-bound **standard MAC** direction under a
per-tracker/per-gateway/per-generation custody key when ACK confidentiality is
not needed. Exact primitive/KDF/truncation/bytes remain unfrozen and require
vectors/audit. Counter-based AEAD remains possible only if reboot/re-ACK/wear
proof succeeds.

Status for M1-M6: **FIXES APPLIED; FOCUSED RE-VERIFICATION REQUIRED.**

---

## 5. LOW dispositions

- **L1 — stale History v3 wording:** active AGENTS guidance corrected to
  History v4 with four bounded state slots. Historical v2/v3 migration text is
  retained where it intentionally describes the old format.
- **L2 — offline authorization scope creep:** custody document now points to the
  delegated application-security direction instead of defining a second
  permission system.
- **L3 — reboot dedupe-index cost:** SF4B must budget bounded RAM and boot/full
  scan reconstruction time.
- **L4 — reboot-created opaque duplicates:** included explicitly in gateway
  capacity/wear accounting.
- **L5 — tracker fingerprint crypto coexistence:** SF4C/E is gated by the
  existing CC310/Bluefruit ownership/readiness rules if cryptographic
  fingerprint/MAC work runs on RAK4630.

Status: **FIXES APPLIED; FOCUSED RE-VERIFICATION REQUIRED.**

---

## 6. Evidence boundary after fixes

These fixes are documentation/architecture changes only.

They do **not** establish:

- gateway custody runtime;
- a frozen custody ACK wire format;
- a chosen custody MAC/KDF or fingerprint length;
- a gateway flash partition or queue format;
- host-test PASS for a future CustodyStore;
- RAK4630 build impact;
- physical Tracker -> Gateway custody;
- SoftDevice asynchronous custody-write physical behavior;
- Gateway -> Edge or Edge -> Backend implementation;
- measured RF capacity, flash endurance or regulatory compliance.

TLP v1 bytes and current SF1-SF3 firmware behavior are not changed by this
documentation slice.

---

## 7. Required next gate

Do not merge this SF4A architecture as reviewed/closed yet.

Independent focused re-verification must confirm at minimum:

- B1 delayed-ingest replay/epoch data-loss path is closed;
- H1 authenticated downstream release chain is closed;
- H2 custody scope/drain-rate/airtime/wear gates are sufficient;
- H3 authorized-gateway trust/revocation blast radius is explicit;
- H4 async flash commit-before-ACK invariant is closed;
- M1-M6 and L1-L5 consistency fixes introduced no new blocker/high issue.

Only after focused re-verification may the documentation PR move from audit
FAIL-with-fixes-applied to a mergeable architecture state.


---

## 8. Focused re-verification at ce47f8c

Independent focused re-verification of
`ce47f8cb0ba608f886db217304458885ac205b58` returned:

```text
PASS WITH FIXES
BLOCKER 0
HIGH    0
MEDIUM  1
LOW     3
```

The reviewer confirmed B1, H1-H4, M1-M6 and L1-L5 were substantively closed and
that the first fix set introduced no new BLOCKER/HIGH.

Remaining findings and dispositions:

- **R-M1 retired-epoch compromise boundary:** fixed after ce47f8c by requiring a
  durable retirement acceptance ceiling for planned rotation, quarantine for
  compromise-driven retirement, and content-aware integrity conflict handling
  for same logical identity with different authenticated content.
- **R-L1 opaque Edge wording:** fixed; DURABLE_EDGE_CUSTODY now commits the
  exact opaque protected custody object.
- **R-L2 revocation validation gate:** fixed; after durable floor/generation
  advance, stale/revoked gateway ACK must cause zero tracker-release mutation,
  and residual offline authority must have an explicit reviewed bound.
- **R-L3 status/disposition staleness:** fixed in this disposition, AGENTS and
  the custody document.

Per the reviewer, these were documentation-level corrections and did not
require another broad audit.

### Final short control

Independent short final control of
`ae00e43a5eb2b7c467c1e81b39e6d2d225960493` returned:

```text
PASS
BLOCKER 0
HIGH    0
MEDIUM  0
LOW     0
```

The reviewer confirmed R-M1 and R-L1-R-L3 are closed and that the residual
fixes introduced no new BLOCKER/HIGH/MEDIUM issue.

This closes the SF4A **documentation/architecture** audit only. It does not
approve wire bytes, MAC/KDF/fingerprint parameters, flash partition/queue
format, RF timing/regulatory compliance, runtime behavior or physical custody
evidence.
