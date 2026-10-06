# ORUN History / Store-Forward Delivery Contract

Status: **SF0/SF1/SF2 FOUNDATION COMPLETE; M4P4 SF2 WIRE FROZEN; SF3 DEVICE-SIDE RUNTIME ACTIVE; SF4 GATEWAY DURABLE CUSTODY DIRECTION UNDER REVIEW.**

Baseline for the SF4 custody amendment: `main@572f92cdb664150ac0dfec8394573da71061d1bd`.

This document originally closed the product-level ambiguity around HistoryStore
replay, delivery evidence, multi-gateway ingestion and flash-wear behavior
before production backlog replay. SF3 has since activated that device-side
runtime; SF4 now amends the downstream custody boundary without rewriting the
validated SF1-SF3 foundations.

The original SF0-SF3 contract did not change TLP v1 bytes or activate gateway,
backend or mobile behavior. SF3 is now merged and physically qualified only on
the device-side HISTORY_SECURE replay boundary. The owner-approved next
direction is recorded in
`docs/architecture/ORUN_GATEWAY_DURABLE_CUSTODY.md`; that SF4 amendment does
not authorize production custody runtime until its storage/security contract is
independently reviewed.

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

## 4. Trusted delivery scopes and SF4 custody amendment

ORUN keeps delivery scopes explicit:

1. **RF_RECEIPT** — some receiver heard the frame.
2. **DURABLE_GATEWAY_CUSTODY** — an authorized gateway durably committed the
   exact opaque protected custody object to its reviewed local queue.
3. **DURABLE_EDGE_CUSTODY** — an authorized phone/Pi/Linux Edge durably
   committed the exact opaque protected custody object to its larger local
   queue.
4. **BACKEND_DURABLE** — the canonical backend durably accepted the observation.
5. **APPLICATION_DELIVERED** — a family-specific endpoint condition, used by
   services such as MESSAGE where backend custody alone is insufficient.

SF0-SF3 originally used the conservative rule that only authenticated
BACKEND_DURABLE evidence could advance tracker delivery state. SF4 intentionally
changes that product boundary:

> After a gateway has durably committed the exact protected custody object, a
> valid authenticated **GATEWAY_CUSTODY_ACK** may release the tracker from
> further RF replay only when the tracker can unambiguously map that ACKed
> object to one retained History observation.

RF receipt or volatile RAM custody is still insufficient. The ACK is
authoritative only after the gateway's persistent queue commit is complete. If
the gateway queue is full, faulted, or the write outcome is unknown, it must not
ACK and the tracker remains responsible.

This is a **chain-of-custody** transfer, not a claim that the data already
reached the backend:

```text
Tracker -> Gateway durable custody -> Edge durable custody -> Backend durable
```

Each owner may release its own copy/responsibility only after the next owner has
durably accepted the same logical observation.

BACKEND_DURABLE remains the canonical backend/application fact and keeps the
existing backend-authority security meaning. A gateway must still never mint a
BACKEND_DURABLE receipt.

The custody ACK security authority is separate from both backend authority and
application COMMAND authority. A forged custody ACK could suppress tracker
replay, so custody ACK requires authentication, anti-replay/revocation,
explicit custody authorization, and binding to the exact protected custody
object. The tracker must map that object to one retained History record before
release-state mutation. The opaque gateway is not required to decrypt the
current encrypted HistoryRecordIdentity and must not receive tracker `K_root`.

Exact custody wire bytes, security context/key derivation and flash partitioning
remain unfrozen until SF4 independent review. See
`ORUN_GATEWAY_DURABLE_CUSTODY.md`.

---

## 4.1 SF4 tracker-release watermark

The original persistent field/API name `delivered_through` was created while
BACKEND_DURABLE was the only trusted release fact. SF4 introduces an earlier
durable responsibility transfer at the gateway.

The conceptual SF4 watermark is therefore **tracker_release_through**:
the oldest contiguous History prefix whose responsibility has been durably
transferred away from the tracker. It must not be presented as proof that every
record is already BACKEND_DURABLE.

SF4A does not change HistoryStore bytes or APIs. SF4E must explicitly review
whether the existing durable field is semantically migrated/renamed or whether
a distinct state is required. No silent reinterpretation and no per-record
flash write is authorized.

---

## 5. Replay ordering and receipt rule

The **safety invariants** are frozen here; exact replay batching/scheduling is
not.

1. live/critical work has priority;
2. replay selection starts from the **oldest undelivered actual History
   record**;
3. retransmission of a logical observation keeps the same History observation
   identity;
4. an authenticated receipt must name one or more **explicit logical
   observation identities** and bind the correct tracker, History incarnation
   and security lifetime;
5. authenticated delivery facts may be accumulated in a **bounded RAM
   acknowledged-ID set** even if they arrive for a newer live/stored record;
6. durable `delivered_through` advances only while the oldest remaining actual
   History records form a contiguous locally-confirmed prefix;
7. a numeric gap caused by ticket reservation/reboot is never treated as an
   implicitly delivered observation.

Do **not** accept a raw `delivered through sequence N` receipt or a remote
numeric range and blindly erase all lower records. Ticket reservations, reboot
skips and other sequence users create numeric gaps.

The previous one-distinct-outstanding stop-and-wait idea remains a valid
**candidate sender policy**, but it is **not** frozen as the only production
receipt shape. SF2 may choose a bounded explicit-identity batch receipt when that
materially improves airtime/latency. Such a batch must list bounded exact
observation identities; it must not become a cumulative sequence ACK. SF3 then
owns the bounded RAM selective set and contiguous-watermark algorithm.

### Live/current transmission relationship

A live POSITION remains store-first and its live RF transmission is
**non-blocking with respect to BACKEND_DURABLE receipt**: the tracker does not
hold the current tracking path open waiting for one receipt per live packet.

If backend durable evidence for a currently stored live record arrives later,
SF3 may place that explicit identity into the same bounded RAM acknowledged-ID
set. Under SF4, an independently reviewed authenticated gateway-custody fact may
feed the same logical release mechanism once that runtime is implemented.
BACKEND_DURABLE names explicit logical History identities after decrypt;
GATEWAY_CUSTODY_ACK instead names/binds the exact opaque custody object and the
tracker performs the object -> retained-record mapping. No numeric range is
trusted. If a selective fact is lost on reboot before a coarse durable
checkpoint, duplicate replay is allowed and downstream dedupe makes it safe.

The exact RAM-set bound, batch bound, retry policy and sender concurrency remain
SF2/SF3 decisions and are gated by the throughput/airtime proof in §9.

---

## 6. Flash-wear and durable checkpoint policy

Current HistoryStore format v4 retains four bounded state slots per active
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

- four History state slots per page;
- normal record-driven page rotation;
- outage backlog size;
- reboot frequency;
- duplicate replay cost;
- History flash erase/write budget;
- SecurityStore A2D replay-reservation writes used to admit authenticated
  backend receipts;
- SecurityStore D2A TX-reservation writes used by protected historical uplink
  traffic.

Current reviewed implementation facts are
`kA2dReplayReservationBlockSize = 8`,
`kSecurityStateSlotsPerPage = 99`, and
`kTxReservationBlockSize = 256`. They are engineering inputs to the SF2/SF3
wear model, not permission to move per-record History writes into SecurityStore
and ignore total erase cost.

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
- an identity may enter the authenticated RAM release watermark only while it
  corresponds to an actual retained History record, not merely a numeric
  sequence/ticket bound; once admitted from an authorized durable release fact
  (BACKEND_DURABLE in SF0-SF3, or reviewed DURABLE_GATEWAY_CUSTODY in SF4), a
  later capacity-driven erase does not revoke that fact, so the same RAM
  watermark may still be checkpointed even if that record is no longer
  physically retained;
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

The secure D2A observation/replay contract must carry or unambiguously bind
enough context to represent at least:

- logical History observation identity;
- source device / credential lifetime;
- observation time plus time quality, or explicit UNKNOWN time;
- application family/schema;
- historical/replay context;
- authenticated receipt correlation.

M4P4 / SF2 carries the independently reviewed and final-verified **frozen** compact wire for this
requirement under TLP v2 `HISTORY_SECURE` type `0x03`:

- 36-byte authenticated header;
- DEVICE_D2A header context `0x02` for historical observation;
- BACKEND_A2D header context `0x01` for BACKEND_DURABLE receipt;
- header context codes remain distinct from M7P6D KDF/nonce direction bytes
  (D2A `0x01`, A2D `0x02`);
- History incarnation is authenticated in the header;
- History record identity is explicit in protected plaintext;
- receipt lists 1..6 explicit identities and never a cumulative numeric range.

M4P4 post-audit revalidation and independent final verification are complete.
These exact bytes are the canonical SF2 wire freeze.

For UNKNOWN observation time, backend/application code must preserve UNKNOWN.
Gateway/backend receive time may be stored separately as path/ingestion
metadata; it must never be substituted as observation time.

---

## 8. Stable observation identity / history incarnation gate

Current HistoryStore has a 64-bit local `identity`, while TLP v1 exposes only
the low uint32 sequence. A destructive History partition reinitialization can
restart the local identity namespace.

Before production backend dedupe/receipt is frozen, ORUN therefore needs one
explicit **History observation-stream incarnation** rule so an old record and a
post-maintenance new record can never collide merely because both use the same
DeviceIdentity and local History identity.

SF1 must choose one reviewed incarnation mechanism that survives the exact
failure it is meant to disambiguate:

- a cryptographically random value of at least 64 bits generated from the
  project's approved CSPRNG and durably committed before the first record of the
  new History stream becomes authoritative; or
- a monotonic value durably owned outside the History erase/re-baseline region,
  so destructive History reinitialization cannot roll it back.

The incarnation is semantically owned by History observation identity even if
its monotonic durability lives in another reviewed persistence owner. Do not
derive it from gateway identity, boot uptime, phone time or a wrapping v1
sequence. A monotonic counter stored only inside the partition being erased does
**not** satisfy this rule.

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

M4P3 selects the first concrete implementation candidate for this rule:

- History journal format advances from v3 to **v4**;
- each committed page header carries one nonzero 64-bit
  `history_incarnation` at bytes 24..31;
- the existing 64-byte static header size, 352-byte page header, 36-byte record
  size and 728-record capacity remain unchanged;
- a blank/re-baselined History partition obtains its incarnation from the
  RAK4630/nRF52840 hardware CSPRNG before SoftDevice startup;
- the page header is committed before any record on that page can become
  authoritative;
- every valid page in one recovered stream must carry the same incarnation;
  conflicting committed incarnations fail closed;
- reboot and capacity-driven page rotation preserve the recovered incarnation;
- entropy failure on a blank partition fails closed without writing flash;
- retained v2/v3 development History is **not** automatically migrated or
  assigned a synthetic incarnation. It remains untouched and requires an
  explicit full History development reset/re-baseline before v4 can start;
- mixed v3/v4 evidence is also a reset boundary rather than a partial
  migration path.

This is a deliberate clean development cutover. No deployed customer fleet
currently depends on History format v3, so speculative in-place migration is
not justified.

After a device has committed any v4 History page, do **not** flash a pre-v4
History firmware while retaining that v4 partition. The older implementation
does not recognize v4 as authoritative History evidence and can create a new v3
page zero, destroying part of the v4 stream. If downgrade is required, first
erase the complete History region `0xED000..0xF3FFF` and verify by readback
that the full region is erased. That operation is an explicit destructive
development re-baseline, not migration.

Because this changes persistence format and physical recovery behavior, M4P3
still requires focused storage review and physical qualification before the v4
cutover is claimed complete.

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

For the activated SF3 runtime, **authenticated downstream contact** means a
successfully authenticated backend-authority A2D frame under the currently
accepted security lifetime; an arbitrary gateway frame is not backend contact.
SF4 may add a separately authenticated custody-capable gateway contact signal,
but only under the reviewed custody authority contract. Mere RF reception or
ordinary delegated COMMAND authority never counts. When trusted downstream
contact is absent, backlog probing remains bounded rather than becoming a tight
retry loop.

### Backlog-drain feasibility gate

SF2 wire freeze is blocked until its candidate historical-observation/receipt
shape demonstrates that backlog service capacity can exceed the claimed nominal
record-production rate under the current rendezvous model.

The model must include, at minimum:

- the 10-second TRACKER post-TX development RX window and the fact that a
  worst-case current relayed uplink can reach a gateway about 5.433 seconds
  after TRACKER TX completion before processing/Internet latency;
- final/candidate protected uplink and receipt airtime;
- direct and selected-relay return paths;
- gateway half-duplex occupancy from receipt downlinks;
- relay amplification/turnaround;
- backend durable-commit/return latency that may push a receipt into a later RX
  opportunity rather than the same 10-second window;
- live/critical traffic priority;
- retry/backoff and expected loss;
- configured tracking interval(s) for which the product claims backlog
  recovery;
- History capacity and the possibility of new records being created while the
  backlog drains.

The frozen SF2 backend receipt shape and the activated SF3 replay policy retain
their existing evidence. SF4 must run a new airtime/rendezvous calculation for
the candidate custody ACK shape before custody runtime activation, including
gateway half-duplex occupancy, relay delay, tracker RX-window timing and loss.

No regulatory duty-cycle percentage is hard-coded by this contract.

SF4's initial custody scope is protected History backlog, not legacy live
POSITION. Once trusted custody contact exists, the connected drain policy must
demonstrate:

```text
confirmed custody drain rate > retained-record production rate
```

with documented margin. The model must also include a quantitative gateway
flash-wear budget; merely proving RF airtime is insufficient. Candidate bounded
multi-object/delayed ACK and Edge-connected fast handoff may be evaluated in
SF4C/D, but none is frozen here.

The nominal engineering load remains roughly 30–50 active devices with about
100 in one RF collision domain as the current stress target. A backend fleet may
contain many more devices because fleet size and one RF collision domain are
different quantities.

---

## 10. Security / receipt requirements

Any receipt that releases tracker replay state can eventually allow old History
records to be overwritten. It is therefore security-sensitive.

### BACKEND_DURABLE

The existing SF2 BACKEND_DURABLE receipt remains a backend-authority A2D fact.
Its path must provide:

- origin/authority authentication by the backend-authority A2D security owner;
- target-device binding;
- credential/incarnation binding;
- explicit observation identity binding;
- anti-replay.

A BACKEND_DURABLE receipt is an idempotent durable fact, not a wall-clock-fresh
command. Its application validity must not expire merely because an offline
gateway uploads hours later. Transport anti-replay is enforced by the reviewed
A2D counter/security layer and credential/epoch rules. If a valid fact arrives
behind a newer accepted A2D counter, backend may reissue the same logical fact
under a fresh security counter; the observation identity does not change.

### Custody-delayed DEVICE_D2A observation acceptance

Tracker -> Gateway custody can release the tracker before backend ingest.
Therefore the backend must not use the generic bounded DEVICE_D2A replay window
as a hard rejection rule for authenticated immutable HISTORY_SECURE
observations arriving later from Gateway/Edge custody.

For this observation family only:

- authenticate AEAD under the credential/epoch that originally protected the
  object;
- accept a valid old security counter even when it lies behind the ordinary
  bounded reordering window;
- dedupe the decrypted application observation by
  `(DeviceIdentity, HistoryIncarnation, HistoryRecordIdentity)`;
- treat old-counter repeats as idempotent duplicate observations/abuse signals,
  not as side-effecting commands.

The backend/authority may retain retired D2A decrypt material while custody
objects from that credential/epoch remain unresolved, but automatic canonical
acceptance of that retired lifetime is bounded by a durable retirement
acceptance ceiling recorded by the security authority.

For planned rotation, authenticated old-epoch frames above the authoritative
maximum counter/range that could legitimately have been originated before
retirement are rejected. If retirement is compromise-driven, late old-epoch
objects enter quarantine/recovery instead of canonical History automatically.

Backend dedupe is content-aware: the same logical History identity with
equivalent authenticated observation content is idempotent; the same identity
with different authenticated content is an integrity conflict and must not be
silently resolved as "first arrival wins".

This family-specific exception is recorded in
`ADR_M7P6_SECURITY_ARCHITECTURE.md` and does not weaken replay/freshness rules
for COMMAND, RESULT or other mutations.

### GATEWAY_CUSTODY_ACK

SF4 adds a separate authority statement: an authorized gateway proves that the
exact opaque protected custody object is in its own reviewed durable queue. The
gateway is not required or permitted to infer the encrypted logical
HistoryRecordIdentity.

This ACK must provide the custody-object binding and revocation/replay
properties defined in `ORUN_GATEWAY_DURABLE_CUSTODY.md`. The tracker must
resolve that authenticated custody object to one retained History record before
advancing release state. The ACK must **not** reuse BACKEND_DURABLE authority,
must not give a gateway tracker `K_root`, and must not make ordinary delegated
COMMAND authority sufficient to release History.

Exact custody security context, key derivation, counter ownership and bytes
remain unfrozen pending focused independent review.

Do not add an unauthenticated TLP v1 ACK.

A receipt means only the scope it states. Neither custody nor backend receipt may
be reused as COMMAND `RESULT`, human MESSAGE `DELIVERED`, user READ state or
generic LOST contact without an explicit service rule.

---

## 11. Gateway / Edge / backend behavior

Any compatible authorized fixed or MOBILE gateway may durably accept a received
opaque custody object. A tracker is not assigned to one gateway.

The intended custody chain is:

1. gateway receives the complete opaque HISTORY_SECURE protected frame;
2. gateway commits that exact custody object to its power-cut-safe local queue;
3. flash success completion + readback/integrity/commit verification must finish
   before gateway may return authenticated GATEWAY_CUSTODY_ACK bound to that
   object;
4. tracker accepts the ACK only after security checks and unambiguous mapping
   from the ACKed object to one retained History record, then stops RF replay
   for that logical observation;
5. gateway retains the exact opaque custody object until an **enrolled Edge**
   durably commits that object and returns authenticated, object-bound
   EDGE_DURABLE_ACCEPT evidence;
6. Edge retains the object through Internet outage until **authenticated
   backend durable acceptance** (for example server-authenticated TLS plus an
   application result bound to the submitted object/observation);
7. backend authenticates/decrypts the original HISTORY_SECURE object, accepts
   custody-delayed immutable observations under the §10 exception, and dedupes
   by logical observation identity while retaining useful path metadata.

A volatile or unauthenticated BLE/USB/UART transfer from Gateway to Edge is not
durable handoff. Likewise a socket write or unauthenticated success response
from Edge to Backend is not durable acceptance. Custody storage does not itself
grant Edge plaintext authority.

Gateway custody is intentionally a short outage bridge. Long-duration buffering
belongs to Edge storage such as Android SQLite or Pi/Linux disk/database. No
30-60 day internal RAK gateway buffer is assumed. Once an object has been
custody-ACKed, normal gateway capacity pressure may **not** evict it before
authenticated durable Edge handoff; when space is exhausted the gateway refuses
new custody instead.

The first SF4 runtime scope is HISTORY_SECURE backlog replay. Legacy live TLP v1
POSITION remains unchanged. When an authenticated custody-capable gateway is
present, SF4D must provide a bounded drain mode whose confirmed custody service
rate exceeds new History production with documented margin for every product
cadence/topology claimed. The existing one-probe/hour no-contact SF3 policy is
not itself a sufficient connected drain policy.

Gateway-to-gateway complete-site synchronization remains a separate capability
and is not required for the first custody closure.

---

## 12. Failure behavior

### RF disappears during replay
Keep the record undelivered. Retry later under bounded policy.

### Gateway receives then loses Internet
If the gateway has **not** durably committed the exact protected custody
object, tracker remains responsible. If it has durably committed that object
and its authenticated custody ACK was
accepted, tracker does not resume that record merely because Internet is down;
the gateway now retains responsibility until durable Edge handoff.

### Gateway commits but custody ACK is lost
While the protected frame remains available in RAM, the tracker should
retransmit that same frame byte-for-byte across ordinary retry/backoff cycles. Gateway can dedupe that opaque custody object and reissue
an ACK. After tracker reboot, a fresh protected frame for the same logical
observation may be different; downstream trusted owners dedupe by logical
observation identity.

### Backend stores but authenticated Edge-facing acceptance is lost
Edge retains/retries the same opaque object. Backend dedupes the decrypted
logical observation and returns authenticated durable acceptance again.

### Tracker reboots before durable delivery checkpoint
Previously receipted records after the last checkpoint may replay again. This is
safe duplicate work.

### Tracker reboots after durable checkpoint
Resume from the next actual record after the durable checkpoint.

### Two gateways receive the same protected object
Both may hold/upload the same opaque object; exact-object dedupe is byte-based at
custody owners, while backend decrypts and stores one logical observation with
multiple path observations if useful.

### Malicious/forged receipt
Authentication/anti-replay failure; no History delivery state mutation.

### Tracker storage wrap before trusted release
Current circular overwrite behavior remains a capacity loss mode: physical page
rotation is capacity-driven and is **not** gated by `delivered_through`.
A delivery checkpoint controls logical replay progress; it is not current
physical erase authorization.

If an unconfirmed or currently replay-outstanding record is overwritten by
capacity rotation:

- count/report the observation as **capacity loss**, not delivered data;
- discard any RAM outstanding/selective-receipt state for the missing record;
- ignore a **new** later receipt for that no-longer-present identity for History
  mutation purposes;
- reselect the oldest remaining actual undelivered record;
- never newly admit a missing identity merely to bridge the gap.

If a record had already been explicitly authenticated and admitted into the
contiguous RAM release watermark **before** capacity rotation erased it, that
trusted durable custody/delivery fact remains valid. A later coarse checkpoint may persist that
previously validated watermark; this does not retroactively mark any
intervening unconfirmed/capacity-lost records as delivered. Progress beyond a
capacity-loss gap requires an explicit authenticated receipt for the oldest
remaining actual History record.

Product diagnostics must expose overwrite/backlog pressure and distinguish
confirmed backlog release from capacity overwrite. Increasing tracker retention
remains a separate capacity decision; reviewed gateway custody instead transfers
responsibility earlier and must expose queue pressure/backpressure of its own.
Gateway queue pressure must fail closed by refusing new ACKs, never by silently
dropping already ACKed-but-not-handed-off custody.

---

## 13. Implementation slicing

Do not implement store-forward as one large PR.

### SF0 — this contract
Freeze semantics and audit them. No runtime/wire changes.

### SF1 — History identity / checkpoint foundation
M4P2/M4P3 provide the delivery-progress foundation and physical History v4
observation-stream incarnation cutover. Preserve store-first behavior and flash
wear invariants.

### SF2 — secure historical observation + receipt codec
M4P4 completed the exact-wire slice. Its frozen SF2 contract is:

- TLP v2 type `0x03 HISTORY_SECURE`;
- 73-byte protected historical POSITION observation;
- 56..96-byte BACKEND_DURABLE receipt;
- receipt batch of 1..6 explicit History record identities;
- backend-authority A2D material may authorize BACKEND_DURABLE;
- delegated gateway material may not;
- TLP v1 remains byte-identical.

The security-context registry is canonical and separate from crypto-direction
bytes:

```text
header contexts: 0x01 BACKEND_A2D, 0x02 DEVICE_D2A
KDF/nonce dirs:  0x01 D2A,         0x02 A2D
```

M4P4 post-audit focused revalidation and independent final verification
closed PASS; the exact bytes are frozen.

### SF3 — tracker replay runtime
Oldest-first replay with the §5 bounded sender policy selected after the §9
feasibility gate, live/critical priority, retry/backoff, reboot behavior and
diagnostics.

### SF4 — gateway durable custody + Edge handoff
SF4A defines the owner-approved custody contract for focused independent review.
After that review closes, subsequent slices may add the bounded power-cut-safe
gateway queue, authenticated custody ACK, tracker custody-receipt integration,
then durable Gateway -> Edge handoff. Exact wire/security/partition choices
remain blocked until the review is closed.

### SF5 — backend synchronization + end-to-end physical qualification
Prove tracker -> gateway durable write -> authenticated custody ACK -> tracker
replay stop -> gateway reset recovery -> Edge durable handoff -> Internet outage
-> backend synchronization -> duplicate/retry convergence using real RAK4630 RF
timing where hardware behavior is claimed.

---

## 14. Validation gates

Before claiming store-forward complete:

- existing M4/R1 History fault tests remain PASS;
- TLP v1 compatibility/golden fixtures remain byte-identical;
- new identity/incarnation recovery and power-cut tests PASS;
- metadata/checkpoint wear tests prove **zero metadata-only History page
  erases**, including append-free receipt/checkpoint sequences after all four
  state slots have been consumed;
- SecurityStore mutation/erase accounting covers A2D replay reservation and D2A
  TX reservation under representative backlog-drain workloads;
- destructive History re-baseline creates a non-colliding incarnation before
  the first new record, including power-cut/reboot boundaries;
- replay selection ignores/re-baselines legacy persistent `replay_cursor` so
  stale cursor state cannot skip undelivered records;
- replay priority/backoff/contact-probe tests PASS;
- live-record receipts plus bounded RAM selective acknowledgement advance the
  durable watermark only through a contiguous actual-record prefix;
- duplicate backend ingest and lost-receipt tests PASS;
- an unenrolled/unauthorized custody ACK, including one authenticated only with
  ordinary delegated COMMAND authority, is rejected and causes **zero History
  release-state mutation**;
- authenticated backend-authority BACKEND_DURABLE replay/forgery tests remain
  PASS;
- custody ACK replay/forgery/revocation tests PASS before SF4 runtime activation;
- capacity overwrite of an outstanding/unconfirmed record increments loss
  diagnostics and cannot be converted into delivery by a late receipt;
- candidate then exact-wire backlog-drain/airtime model passes with margin for
  the tracking intervals/topologies the product claims;
- RAK4630 production build PASS;
- physical direct and relay outage/recovery path PASS;
- fixed gateway and MOBILE gateway paths both prove the same application
  observation identity and backend convergence semantics.

Host/build PASS alone is not physical store-forward proof.

---

## 15. Explicit non-claims

This document does not claim that:

- gateway durable custody is implemented today;
- the SF3 single-device physical qualification proved gateway reception,
  gateway persistence, custody ACK, Edge handoff or backend ingest;
- any gateway can stop tracker replay merely by hearing a packet;
- a GATEWAY_CUSTODY_ACK wire format/security context is frozen;
- gateway internal-flash queue size/partition ownership has been selected;
- 30-60 days of buffering belong on the RAK gateway;
- Edge/mobile/backend synchronization is implemented;
- current History metadata supports per-record durable delivery writes;
- TLP v1 provides authenticated delivery.

SF3 device-side HISTORY_SECURE replay is implemented and was physically
qualified only within that stated single-device evidence boundary.

---

## 16. Independent audit disposition boundary

The recovered independent M4P1 architecture/security audit returned
**PASS WITH FIXES** with:

- 0 BLOCKER
- 2 HIGH
- 4 MEDIUM
- 5 LOW

The audit transcript did not print its exact head SHA in the recovered summary.
Repository chronology shows that the audited pre-hardening PR state had four
branch commits and preceded the post-audit hardening commits. This document
therefore does not mislabel the later head as already independently reviewed.

Disposition implemented in this revision:

- **H1** backend-only A2D authority for **BACKEND_DURABLE**; delegated gateway
  material cannot mint or impersonate that backend fact. SF4's later
  custody-specific authority is a distinct scope and is not covered by the old
  audit;
- **H2** stop-and-wait removed from frozen semantics; bounded explicit-ID batch
  receipts allowed; candidate/exact throughput gate added before runtime;
- **M1** no metadata-only History rotation/erase; explicit zero-erase test gate;
- **M2** SecurityStore A2D/D2A reservation wear included;
- **M3** incarnation source must survive History re-baseline and commit before
  first new record;
- **M4** live transmission is non-blocking; authenticated live delivery facts
  may feed a bounded RAM selective set while durable progress remains contiguous;
- **L1** baseline corrected to current main;
- **L2** persistent replay cursor excluded from the production replay contract;
- **L3** outstanding-record capacity overwrite becomes explicit loss/reselect
  behavior;
- **L4** durable receipt facts are not invalidated by wall-clock delay;
- **L5** authenticated downstream contact/probe semantics defined.

Focused independent final verification of the original SF0-SF3 contract
returned **PASS WITH MINOR DOC FIX**: no BLOCKER/HIGH/MEDIUM remained; R1-R4
were documentation-only consistency corrections.

That audit does **not** approve the later SF4 gateway-custody authority change.
The SF4 amendment changes who may release tracker replay state and adds a new
persistent custody owner, so it requires a focused independent
security/storage/airtime review before custody wire bytes or runtime are
merged.
