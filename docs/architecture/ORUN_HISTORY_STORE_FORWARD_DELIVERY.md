# ORUN History / Store-Forward Delivery Contract

Status: **DRAFT IMPLEMENTATION CONTRACT — DOCUMENTATION ONLY; independent architecture/security audit required before runtime or wire implementation.**

Baseline: `main@c88516615ec956abc3a079d625b034dc2c2c34aa`.

This document closes the product-level ambiguity around HistoryStore replay,
delivery evidence, multi-gateway ingestion and flash-wear behavior before the
first production backlog replay path is implemented.

It does **not** change TLP v1 bytes, HistoryStore format v3, RF behavior,
SecurityStore, gateway runtime, backend behavior or mobile behavior.

The product requirement is simple:

> Important tracker observations survive temporary RF / phone / Internet loss
> and are transferred later without allowing backlog replay to masquerade as
> live data, erase unconfirmed records, exhaust flash metadata, or become tied
> to one particular gateway.

---

## 1. Product outcome

Store-forward is complete only when ORUN can demonstrate this chain:

```text
observation
 -> durable tracker history
 -> live attempt when appropriate
 -> outage / no trusted receipt
 -> retained backlog
 -> later compatible fixed or mobile gateway path
 -> durable downstream acceptance
 -> authenticated receipt returned to tracker
 -> bounded durable delivery checkpoint
```

A local radio `TX_DONE`, relay forwarding completion, gateway RF receipt,
BLE buffering or backend TCP/HTTP submission by itself is not application
delivery.

---

## 2. Non-negotiable concept separation

Keep these separate:

```text
stored
!= transmitted
!= RF received
!= relay custody
!= gateway custody
!= backend durable acceptance
!= application delivery
```

Also:

```text
History record identity
!= TLP v1 uint32 sequence
!= secure-envelope counter / nonce
!= gateway path identity
!= backend receipt ID
```

The application-level logical observation identity is:

```text
(DeviceIdentity, HistoryIncarnation, HistoryRecordIdentity)
```

`credential_id` and `key_epoch` authenticate a security lifetime and the
receipt/transport carrying an observation, but they are **not** part of the
logical observation identity. Ordinary credential/key rotation must not mint a
second backend observation row for the same stored History record.

And:

```text
replay progress in RAM
!= durable delivered checkpoint
```

A transport retry must not mint a new logical observation merely because it
uses another gateway or another secure transport counter.

---

## 3. Multi-gateway rule

A tracker backlog is **not assigned to one gateway**.

The same observation may be heard through:

- a fixed gateway directly;
- one or more relays and a fixed gateway;
- a MOBILE / roaming gateway;
- a phone-connected gateway that uploads later.

Backend ingestion must collapse duplicate application observations while
retaining useful path observations separately, for example gateway ID,
direct/relay path, relay ID, RSSI/SNR and reception time.

Therefore:

```text
duplicate transport observation = acceptable / useful
duplicate application history row = not acceptable
```

Fixed gateway and MOBILE gateway are capability/profile variations, not
different observation identities.

---

## 4. Initial trusted delivery scope

ORUN needs explicit receipt scopes.

Candidate scopes are:

1. **RF_RECEIPT** — some receiver heard the frame.
2. **DURABLE_CUSTODY** — a reviewed downstream custodian durably stored it.
3. **BACKEND_DURABLE** — the canonical backend durably accepted the observation.
4. **APPLICATION_DELIVERED** — a family-specific endpoint condition, used by
   services such as MESSAGE where backend custody alone is insufficient.

For the first tracker POSITION/history store-forward implementation,
**only authenticated BACKEND_DURABLE evidence may advance tracker durable
`delivered_through`**.

A future physically qualified gateway-custody store may later be authorized to
release tracker history earlier, but gateway RF receipt or volatile RAM custody
must never do so.

This conservative first rule permits any compatible gateway path to carry the
observation and any compatible return path to carry the receipt while avoiding
data loss when an intermediate gateway disappears before synchronization.

---

## 5. Replay ordering and receipt rule

The initial tracker replay algorithm is intentionally simple and bounded:

1. live/critical work has priority;
2. choose the **oldest undelivered actual History record**;
3. expose at most one distinct backlog observation as outstanding for delivery
   confirmation at a time;
4. retransmission of that logical observation keeps the same History observation
   identity;
5. accept only an authenticated receipt that names the exact outstanding
   observation identity and correct tracker/credential context;
6. advance an in-RAM contiguous delivered watermark only after that receipt;
7. then move to the next actual History record.

This oldest-first stop-and-wait rule makes cumulative durable checkpointing safe
without assuming that every numeric sequence/ticket value existed as a History
record.

Do **not** accept a raw `delivered through sequence N` receipt from a remote
peer and blindly erase all lower records. Ticket reservations, reboot skips and
other sequence users create numeric gaps.

Out-of-order/selective ACK optimization may be added later only with a separately
reviewed bounded bitmap/range contract.

---

## 6. Flash-wear and durable checkpoint policy

Current HistoryStore format v3 has only four 32-byte state slots per active
page. Calling `markDeliveredThrough()` after every acknowledged record would
consume metadata slots far faster than normal record appends and can force
metadata-driven page rotation/erase while useful record capacity remains.

Therefore **per-record durable delivery writes are prohibited**.

Initial policy:

- receipt progress is accumulated in RAM as a contiguous acknowledged watermark;
- durable `delivered_through` is checkpointed only at bounded coarse points;
- a reboot before the next checkpoint may cause already delivered observations
  to be replayed again;
- that duplicate replay is acceptable and must be deduplicated downstream;
- data loss or premature deletion is not acceptable.

The runtime implementation milestone must choose and test an exact checkpoint
policy against:

- four state slots per page;
- normal record-driven page rotation;
- outage backlog size;
- reboot frequency;
- duplicate replay cost;
- flash erase/write budget.

A checkpoint policy must not create metadata-driven history destruction merely
to reduce duplicate retransmission.

The existing persistent `replay_cursor` field/API is **not** authorization to
persist every replay attempt. The initial runtime uses a RAM replay cursor.
Any durable use of `replay_cursor` requires the same wear analysis.

Two current implementation details are explicit hazards for SF1/SF3:

1. current `markDeliveredThrough()` calls `startState()`; when the active
   page's four state slots are exhausted, `startState()` can call
   `startNewPage(false)`, causing a page erase/rotation solely to persist
   metadata;
2. current `getNextBacklog()` chooses after
   `max(replay_cursor, delivered_through)`, so an old persisted replay cursor
   can skip otherwise-undelivered records.

Therefore the production store-forward path **must not** blindly reuse either
behavior.

SF1 must enforce all of the following:

- the durable checkpoint may never advance beyond the highest contiguous
  authenticated RAM receipt watermark;
- a checkpoint identity must correspond to an actual History record, not merely
  a numeric sequence/ticket bound;
- a delivery checkpoint must **not initiate page rotation or erase solely
  because state slots are exhausted**;
- if no safe state slot exists, durability is deferred until a normal
  record-capacity-driven page transition can carry the checkpoint, or the
  reviewed History format is revised to provide a safe state area;
- SF3 backlog selection starts from durable `delivered_through` plus a
  RAM-owned replay cursor. The current persistent `replay_cursor` /
  `getNextBacklog()` behavior is not a production replay contract and must be
  removed, ignored after an explicit development re-baseline, or replaced by
  the reviewed SF1 API before replay is enabled.

Duplicate replay after reboot remains preferable to metadata-driven record loss.

---

## 7. Historical/live semantics

A backlog record is historical even if it is transmitted during a current
radio session.

Backend/app presentation must use observation-time provenance, never gateway or
backend receipt time, to decide whether a point is live, stale or historical.

TLP v1 POSITION has no explicit historical/replay marker and some valid stored
records may lack trustworthy UTC. Therefore the production store-forward path
must not simply blind-replay v1 POSITION bytes and let consumers treat arrival
as freshness.

The future secure D2A observation/replay contract must carry or unambiguously
bind enough context to represent at least:

- logical History observation identity;
- source device / credential lifetime;
- observation time plus time quality, or explicit UNKNOWN time;
- application family/schema;
- historical/replay context;
- authenticated receipt correlation.

Exact wire bytes are deliberately not frozen here.

---

## 8. Stable observation identity / history incarnation gate

Current HistoryStore has a 64-bit local `identity`, while TLP v1 exposes only
the low uint32 sequence. A destructive History partition reinitialization can
restart the local identity namespace.

Before production backend dedupe/receipt is frozen, ORUN therefore needs one
explicit **History observation-stream incarnation** rule so an old record and a
post-maintenance new record can never collide merely because both use the same
DeviceIdentity and local History identity.

Acceptable implementation directions include a random or monotonic History
incarnation owned by History persistence. Do not derive it from gateway identity,
boot uptime, phone time or a wrapping v1 sequence.

The incarnation lifecycle is part of the contract:

- normal reboot, page rotation and ordinary key-epoch rotation preserve it;
- ordinary credential rotation/re-provisioning for the **same authorized owner**
  does not by itself create a new logical observation stream if History is
  intentionally retained;
- a destructive History reset/re-baseline creates a new incarnation before the
  first new record becomes authoritative;
- an ownership/tenant transfer must never silently expose retained historical
  location data to the new owner. Such a transfer requires either a reviewed
  same-owner continuity decision by the backend authority or an explicit
  History purge/re-baseline with a new incarnation before the new owner can
  receive historical data.

This keeps security credential lifetime separate from History observation
identity while closing the privacy boundary around reprovisioning.

This requirement may justify a future HistoryStore format revision, but this
documentation slice does not change format v3. Because no deployed customer
fleet currently depends on History format v3, the production cutover may choose
an explicit development reset rather than build speculative migration logic;
that decision requires its own storage review and physical qualification.

---

## 9. Replay admission / airtime

Backlog replay is lower priority than:

1. critical EVENT / alarm / required RESULT traffic;
2. current live tracking data;
3. other explicitly higher-priority current observations.

Backlog must be transferred gradually and must not turn restored connectivity
into an RF burst.

The initial runtime must:

- allow only bounded outstanding backlog work;
- yield to live/critical traffic;
- apply explicit retry/backoff;
- account for relay amplification;
- avoid blind continuous replay when no authenticated downstream contact exists;
- use collision-domain airtime assumptions, not total fleet count alone.

No regulatory duty-cycle percentage is hard-coded by this contract.

The nominal engineering load remains roughly 30–50 active devices with about
100 in one RF collision domain as the current stress target. A backend fleet may
contain many more devices because fleet size and one RF collision domain are
different quantities.

---

## 10. Security / receipt requirements

A receipt that advances durable delivery state changes what data the tracker may
eventually overwrite. It is therefore security-sensitive.

The receipt path must provide:

- origin/authority authentication;
- target-device binding;
- credential/incarnation binding;
- observation identity binding;
- anti-replay;
- bounded freshness/session semantics appropriate to the receipt design.

Do not add an unauthenticated TLP v1 ACK.

The secure receipt family should reuse the reviewed TLP v2 security foundations
rather than invent separate cryptography.

A receipt means only the scope it states. It must not be reused as COMMAND
`RESULT`, human MESSAGE `DELIVERED`, user READ state or generic LOST contact
without an explicit service rule.

---

## 11. Gateway / backend behavior

Any compatible fixed or MOBILE gateway may upload a received observation.

Gateway/backend ingestion must be idempotent by logical observation identity.
It should retain per-path reception evidence separately.

For the initial BACKEND_DURABLE policy:

1. gateway sends observation plus path metadata to backend;
2. backend transactionally/durably accepts or recognizes the same observation;
3. backend creates an authenticated receipt for that logical observation;
4. the receipt may return through any currently valid gateway/downlink path;
5. tracker accepts it only if security/identity checks pass and it matches the
   current oldest outstanding replay record.

An offline gateway may keep opaque observations for later synchronization, but
its volatile receipt is not enough to advance tracker delivery state.

This conservative first closure intentionally has a capacity limitation: while
the backend is unreachable, the tracker continues retaining observations even if
a gateway has heard or buffered them. If the outage exceeds tracker History
capacity, today's circular overwrite policy can still lose the oldest
unconfirmed observations. Product diagnostics must expose that pressure; the
first BACKEND_DURABLE slice must not claim arbitrary-duration Internet-outage
retention. A later reviewed and physically qualified DURABLE_CUSTODY path may
extend that bound without weakening receipt authentication.

Gateway-to-gateway complete-site synchronization is a separate capability and is
not required for the first store-forward closure.

---

## 12. Failure behavior

### RF disappears during replay
Keep the record undelivered. Retry later under bounded policy.

### Gateway receives then loses Internet
Keep tracker record undelivered under the initial policy. Gateway may upload
later.

### Backend stores but receipt is lost
Tracker may resend the same logical observation. Backend dedupes and returns the
same delivery fact again.

### Tracker reboots before durable delivery checkpoint
Previously receipted records after the last checkpoint may replay again. This is
safe duplicate work.

### Tracker reboots after durable checkpoint
Resume from the next actual record after the durable checkpoint.

### Two gateways receive the same record
Both may upload path observations; backend stores one logical observation and
may issue/reissue the same delivery fact.

### Malicious/forged receipt
Authentication/anti-replay failure; no History delivery state mutation.

### Storage wrap before delivery
Current circular overwrite behavior remains a capacity loss mode: physical page
rotation is capacity-driven and is **not** gated by `delivered_through`.
A delivery checkpoint controls logical replay progress; it is not current
physical erase authorization. Product diagnostics must expose overwrite/backlog
pressure and distinguish confirmed backlog release from capacity overwrite.
Increasing retention or adding reviewed durable gateway custody is a separate
capacity decision.

---

## 13. Implementation slicing

Do not implement store-forward as one large PR.

### SF0 — this contract
Freeze semantics and audit them. No runtime/wire changes.

### SF1 — History identity / checkpoint foundation
Resolve the production observation-stream incarnation and implement/test the
RAM contiguous receipt watermark + bounded durable checkpoint policy. Preserve
store-first behavior and flash wear invariants.

### SF2 — secure historical observation + receipt codec
After the relevant TLP v2 compact D2A security contract is ready, freeze exact
historical-observation and authenticated BACKEND_DURABLE receipt bytes with
golden/malformed/security vectors. TLP v1 remains byte-identical.

### SF3 — tracker replay runtime
Oldest-first one-outstanding replay, live/critical priority, retry/backoff,
reboot behavior and diagnostics.

### SF4 — fixed/MOBILE gateway + backend ingestion
Idempotent observation ingest, path metadata retention, durable backend receipt
generation and return through any valid gateway path.

### SF5 — end-to-end physical qualification
Prove outage -> backlog -> restored path -> duplicate/retry -> receipt ->
checkpoint across direct and relay paths, tracker/gateway reboot and real
RAK4630 RF timing.

---

## 14. Validation gates

Before claiming store-forward complete:

- existing M4/R1 History fault tests remain PASS;
- TLP v1 compatibility/golden fixtures remain byte-identical;
- new identity/incarnation recovery and power-cut tests PASS;
- metadata/checkpoint wear tests prove no per-ACK page churn;
- replay priority/backoff tests PASS;
- duplicate backend ingest and lost-receipt tests PASS;
- authenticated receipt replay/forgery tests PASS;
- RAK4630 production build PASS;
- physical direct and relay outage/recovery path PASS;
- fixed gateway and MOBILE gateway paths both prove the same application
  observation identity and backend convergence semantics.

Host/build PASS alone is not physical store-forward proof.

---

## 15. Explicit non-claims

This document does not claim that:

- backlog replay is implemented today;
- a production ACK/receipt wire type exists;
- TLP v1 provides delivery;
- gateway durable custody is implemented;
- backend/mobile synchronization is implemented;
- History format v3 already has a production-safe incarnation identifier;
- current four state slots support per-record delivery persistence;
- any current gateway can erase tracker history merely by receiving a packet.
