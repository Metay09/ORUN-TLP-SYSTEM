# ORUN Gateway Durable Custody / Edge Synchronization Direction

Status: **OWNER-APPROVED SF4A DIRECTION — INITIAL INDEPENDENT AUDIT FAIL (1 BLOCKER / 4 HIGH / 6 MEDIUM / 5 LOW); REQUIRED DOC FIXES APPLIED; FOCUSED RE-VERIFICATION REQUIRED. NO NEW WIRE BYTES, FLASH PARTITION OR PRODUCTION RUNTIME AUTHORIZED YET.**

Baseline: `main@572f92cdb664150ac0dfec8394573da71061d1bd`.

Audit disposition:
`docs/audits/SF4_GATEWAY_DURABLE_CUSTODY_AUDIT_DISPOSITION.md`.

This document records the next store-forward ownership boundary after SF3
activated device-side HISTORY_SECURE replay.

It deliberately separates four responsibilities:

```text
Tracker != Gateway != Edge != Backend
```

A physical product may combine Gateway + Edge in one enclosure, but the logical
owners remain separate.

This direction does not change TLP v1 bytes. It does not yet freeze a custody
ACK wire format, allocate gateway flash, authorize a new security context, or
claim multi-device physical validation.

---

## 1. Product outcome

The user should not pay repeated tracker RF/power cost merely because Internet
is unavailable after an authorized gateway has safely accepted the observation.

The intended chain of custody is:

```text
Tracker History
  -> Gateway durable custody
  -> authenticated GATEWAY_CUSTODY_ACK
  -> tracker RF responsibility ends for that logical observation
  -> Edge durable custody
  -> Backend durable acceptance
  -> application/history
```

Each hop releases its own responsibility only after the next hop has durably
accepted the same logical observation.

Backend durability remains the canonical cloud/application fact. Gateway
custody is a separate earlier delivery scope whose purpose is to release the
battery-constrained tracker from repeated RF replay.

---

## 2. Definitions

### Tracker

Owns sensing/tracking and the original History observation. It stores before
send where required.

### Gateway

Owns the LoRa-side receive/bridge function and a **short-duration durable
custody queue**. A gateway may be fixed or mobile. Gateway capability is not a
Role and does not imply relay forwarding, user identity, backend authority or
command authority.

### Edge

Owns a **larger local durable queue** and synchronization toward backend.
Examples include:

- Android phone;
- Raspberry Pi;
- Linux/OpenWrt host;
- a future combined gateway appliance with equivalent local durable storage.

An Edge may operate with no Internet. It is not the canonical backend merely
because it has storage.

### Backend

Owns canonical long-term ingestion, user/account authorization, fleet state and
application-visible durable history.

---

## 3. Delivery scopes remain distinct

```text
RF_RECEIPT
!= DURABLE_GATEWAY_CUSTODY
!= DURABLE_EDGE_CUSTODY
!= BACKEND_DURABLE
!= APPLICATION_DELIVERED
```

A raw RF receive event, CRC-valid packet, TX_DONE, relay forwarding completion,
BLE notification, socket write or HTTP request completion is not durable
custody.

For tracker History, the new owner-approved direction is:

> A valid authenticated GATEWAY_CUSTODY_ACK may release the tracker from further
> RF replay only when the ACK is unambiguously bound to a custody object that
> the tracker can map to one exact retained History observation, and only after
> the gateway has committed that custody object to its reviewed persistent
> queue.

This intentionally changes the earlier first-slice rule that only
BACKEND_DURABLE could release tracker History. The old rule remains historical
audit evidence for SF0-SF3; this SF4A amendment requires its own independent
security/storage review before production implementation.

---

## 4. Chain-of-custody invariants

### 4.1 Tracker -> Gateway

The gateway may emit GATEWAY_CUSTODY_ACK only after all of these are true:

1. the complete received HISTORY_SECURE frame is structurally admissible for
   opaque custody;
2. the gateway has enough durable queue capacity;
3. the exact custody object has been committed using the reviewed power-cut-safe
   storage protocol;
4. the flash backend has reported successful completion, the committed object
   has passed readback/integrity verification, and the commit seal/marker is
   authoritative after reset;
5. the ACK can be authenticated as coming from an authorized custody-capable
   gateway and is bound to that exact custody object.

With SoftDevice enabled, "program/erase request submitted" is **not** durable
commit. Late completion, error, timeout or otherwise unknown flash outcome must
produce **no custody ACK**.

The first gateway queue design must keep a bounded pre-erased/prepared admission
reserve so ordinary custody ACK latency does not require a page erase on the
tracker's receive-window critical path. If an erase/compaction is required,
custody admission is deferred; the tracker retains/retries. Worst-case flash
completion + readback + ACK airtime must be included in the RX-rendezvous model.

The gateway must **not ACK** when the queue is full, faulted, busy beyond the
admission bound, or the durable write outcome is unknown.

Once the tracker accepts a valid custody ACK and can unambiguously map the
ACKed custody object to one retained logical History identity:

- it stops RF replay of that observation;
- it may advance its local tracker-release prefix according to the reviewed
  coarse History checkpoint policy;
- it does not wait for Internet or BACKEND_DURABLE before considering the RF
  custody responsibility transferred.

Physical History page erasure remains capacity-driven. Custody ACK must not
introduce per-record flash metadata writes or weaken the existing
metadata-only-erase prohibition.

### 4.2 Gateway -> Edge

The gateway retains its custodied **opaque custody object** until an authorized
Edge durably accepts that exact object.

Gateway reclaim requires an authenticated **EDGE_DURABLE_ACCEPT** fact from an
enrolled/authorized Edge (or an authenticated mutually trusted local channel
whose application result has the same semantics). The acceptance fact must:

- bind the exact custody-object identity/fingerprint;
- be replay/idempotency safe;
- be emitted only after the Edge's own power-cut-safe durable commit;
- remain recoverable across Edge reset.

A volatile BLE/USB/UART transfer, process RAM enqueue, unauthenticated local
client response or socket write is not enough and must never trigger gateway
reclaim.

Only after authenticated durable Edge acceptance may the gateway reclaim the
corresponding queue entry. The Edge need not decrypt HISTORY_SECURE merely to
own custody.

### 4.3 Edge -> Backend

The Edge retains the opaque custody object while Internet/backend is
unavailable.

Edge reclaim requires authenticated backend durable acceptance: for example, a
server-authenticated TLS transaction whose application response is bound to the
submitted object/result, or an application-layer authenticated receipt. A mere
TCP/TLS write completion is not enough.

Only after backend durable acceptance (or authenticated idempotent recognition
of the same object/logical observation) may the Edge reclaim its local queue
entry.

The backend is the first mandatory decrypting/deduplicating owner in this
custody path. A separately authorized offline application may gain local
plaintext through the existing delegated application-security architecture;
custody storage itself does not imply decryption authority. See
`ORUN_TLP_V2_DELEGATED_COMMAND_SECURITY_DIRECTION.md` for that separate
authority model.

Internet restoration triggers synchronization; it is not required for local
field collection.

### 4.4 Custody-delayed backend acceptance and key retention

A custody ACK transfers the tracker responsibility **before** backend ingest.
Therefore backend security processing must not later reject an otherwise valid
immutable HISTORY_SECURE observation merely because newer D2A counters arrived
first.

For the HISTORY_SECURE **observation** family only:

- after successful AEAD authentication under an authorized credential/epoch,
  the backend accepts the immutable observation regardless of how far its D2A
  security counter trails the newest observed counter;
- a bounded D2A replay window may remain an abuse/telemetry signal, but it is not
  a rejection criterion for this idempotent store-forward observation family;
- backend dedupe is by the stable logical observation identity after decrypt;
- this exception does **not** apply to COMMAND, RESULT, configuration mutation,
  actuation or any other side-effecting application family.

Credential/key rotation must not strand already ACKed custody. The backend
security authority must retain decrypt-only material for retired D2A
credential/epochs while any authorized Gateway/Edge custody from that lifetime
can remain unresolved. An old epoch may be cryptographically retired for new
origination while still being retained for decrypt-only store-forward ingest.

A retired decrypt key may be destroyed only after all relevant custody-capable
Gateway/Edge owners have durably synchronized/closed that lifetime, or an
explicit operator recovery decision accepts the corresponding data-loss risk.
This is an SF4 exception/extension to the generic M7P6 bounded D2A replay/current
epoch rule and is mirrored in `ADR_M7P6_SECURITY_ARCHITECTURE.md`.

---

## 5. Stable identity, opaque custody object and dedupe

The existing logical History observation identity remains:

```text
(DeviceIdentity, HistoryIncarnation, HistoryRecordIdentity)
```

Custody changes ownership of responsibility, not observation identity.

However, the current 73-byte HISTORY_SECURE observation keeps
`HistoryRecordIdentity` inside authenticated ciphertext. An opaque gateway
that does not possess tracker `K_root` therefore cannot be required to decrypt
or even know the logical History identity.

SF4 must distinguish:

```text
logical observation identity
!= opaque gateway custody object identity
```

A candidate custody-object identity may be a cryptographic fingerprint of the
**entire exact protected frame** (header + ciphertext + authentication tag), or
another compact value exposed by a separately reviewed wire revision. Header
fields, security counter or the 8-byte AEAD tag alone are insufficient because
an opaque gateway cannot verify them and an attacker could substitute different
ciphertext while copying visible fields/tag bytes.

The final identifier must have a reviewed collision/second-preimage security
margin appropriate to adversarial RF input; no algorithm, truncation length or
wire field is frozen by this document.

The tracker may release a History record only when it can unambiguously map the
authenticated custody ACK back to the exact outstanding protected frame for
that retained record.

For the first custody sender policy, keep **one distinct protected
HISTORY_SECURE frame outstanding for a retained record while that RAM object
survives**, and retransmit it byte-identically across ordinary retry/backoff
cycles. Do not re-protect merely because a retry interval/attempt timer rolled
over. Re-protection is reserved for reboot/lost RAM object, credential-lifetime
change, or another explicit security-invalidating condition.
That provides three useful properties:

- lost ACK -> gateway can recognize the same opaque frame and re-ACK it;
- retransmission does not burn a fresh D2A security counter every few seconds;
- gateway can dedupe exact duplicate custody objects without decrypting them.

A reboot may lose that RAM-held protected frame. The tracker may then protect
the same logical History observation again with a new security counter, producing
a different opaque custody object. Gateway/Edge may temporarily hold both;
backend decrypt/dedupe must still collapse them to the same logical observation.

Therefore:

- the same logical observation through two gateways remains one backend
  observation;
- exact duplicate custody objects are idempotent at the gateway; fingerprint
  equality is an index/ACK-binding aid, while duplicate identity is confirmed
  by full object length + byte equality before suppressing a second durable
  queue entry;
- different protected frames for the same logical observation may remain opaque
  duplicates until a trusted decrypting owner dedupes them;
- Edge/backend uploads are idempotent by logical observation identity after
  authentication/decryption;
- retries never mint a new **application** observation identity;
- Gateway -> Edge -> Backend handoff re-verifies the full-object fingerprint
  and length so corruption/substitution cannot silently cross a custody hop.

---

## 5.1 Tracker release watermark semantics

SF0-SF3 named the durable History state `delivered_through` because only
BACKEND_DURABLE could advance it. SF4 changes the product meaning: the tracker
may release RF/storage responsibility earlier on authenticated durable gateway
custody.

Therefore the conceptual SF4 state is:

```text
tracker_release_through
```

It means "the oldest contiguous History prefix for which tracker responsibility
has been durably transferred", not "backend has stored every record in this
prefix".

SF4A does **not** silently reinterpret the existing on-flash
`journal_format::State::delivered_through` bytes. SF4E must make an explicit,
reviewed implementation decision:

- reuse those bytes as the renamed tracker-release watermark with updated API,
  diagnostics and migration/compatibility tests; or
- add a distinct durable state if preserving the old backend-only semantic is
  required.

Whichever option is selected must preserve the four-state-slot wear invariant,
must not add per-record writes, and must keep BACKEND_DURABLE separately
observable downstream.

---

## 6. Security boundary

A forged custody ACK could make a tracker stop replaying data that no real
custodian holds. Custody ACK is therefore security-sensitive.

Required properties:

- authenticated origin;
- authorization specifically for **gateway custody**;
- target tracker binding;
- binding to the exact opaque custody object being acknowledged;
- tracker-side unambiguous mapping from that object to one retained History
  record before release-state mutation;
- replay/duplication safety;
- revocation/re-enrollment behavior;
- no unauthenticated TLP v1 fallback.

Custody authority is **not** BACKEND_DURABLE authority and is **not**
application COMMAND authority. The gateway must not receive tracker `K_root`.

An enrolled custody gateway is a **data-loss trust anchor** for the tracker:
while its custody authority is valid, a compromised or faulty gateway that can
produce a valid ACK can intentionally acknowledge and then discard data. SF4
does not pretend cryptography can prove honest flash behavior after enrollment.

The blast radius must therefore be bounded:

- custody ACK authority is derived at least per
  `(tracker, gateway_device_id, policy_floor, grant_generation)`;
- shared site/fleet/group custody-authentication keys are prohibited;
- revocation/floor advance stops acceptance of **new** custody ACKs from the
  revoked gateway once the tracker learns that change;
- revocation does not invalidate already end-to-end protected custody objects:
  a revoked gateway/Edge may still hand previously held opaque objects
  downstream for backend authentication/dedupe.

The existing delegated-gateway security architecture is the authority/enrollment
foundation so ORUN does not create a second unrelated gateway permission
system. Custody gets its own narrowly scoped capability/key derivation and must
not blindly reuse command-plane quota/counter settings.

For ACK authentication, SF4C should prefer an **idempotent object-bound standard
MAC construction** under the per-tracker/per-gateway/per-generation custody key
when no ACK confidentiality is required. Such an ACK is bound to
`tracker + gateway + generation + full-object fingerprint`; replaying the same
valid ACK can only confirm the same custody object and need not consume a new
flash-backed sender counter on every re-ACK. Exact standard primitive, KDF
label, truncation length and bytes remain unfrozen and require host vectors,
negative tests and independent review.

A counter-based AEAD ACK remains possible only if SF4C proves reboot-safe
counter persistence, re-ACK behavior and wear. No custom cryptography is
authorized.

No fleet-wide custody secret is introduced.

### 6.1 Pre-storage admission threat

The current HISTORY_SECURE inner AEAD is intentionally opaque to the gateway, so
the gateway cannot authenticate arbitrary received observation frames using
`K_root` before storage. Visible `device_id`, epoch/counter and other header
fields are not authentication evidence.

SF4B/C must therefore evaluate a custody-specific **outer admission
authenticator** that a custody-authorized gateway can verify before committing
flash, while leaving the HISTORY_SECURE payload opaque. It must be based on the
same per-tracker/per-gateway/generation custody authority and must not reveal
`K_root`.

If the first implementation cannot authenticate before storage, targeted
per-source quota exhaustion by forged headers remains an explicit non-claim.
Regardless, RAM/flash admission and rate limits must bound the damage, and
untrusted traffic may never evict already ACKed custody.

---

## 7. Multi-gateway behavior

A tracker is not locked to one gateway.

Any currently authorized custody-capable gateway may accept an observation.

If multiple gateways durably store the same protected custody object:

- each may independently hold a copy;
- the tracker may accept the first valid ACK bound to that object;
- exact-frame dedupe is possible without decryption;
- downstream trusted owners still dedupe the logical observation identity;
- no gateway obtains exclusive ownership of the tracker.

If an ACK is lost, the tracker should retransmit the same protected frame during
the current attempt. A gateway that already holds that exact object treats it
idempotently and may reissue an authenticated ACK without a second queue item.
After tracker reboot a newly protected frame for the same logical observation
may be opaque-distinct and is allowed to coexist until downstream logical
dedupe. Revoking one gateway affects future custody authority, not the
backend-validity of opaque objects that gateway already holds.

---

## 8. Capacity and backpressure

Gateway custody is a **short outage bridge**, not the 30-60 day archive.

The long-duration queue belongs on Edge storage such as a phone SQLite database
or Pi/Linux filesystem/database.

Current HISTORY_SECURE observation wire size is 73 bytes. The final gateway
custody record will be larger because it requires queue metadata/integrity
fields. A prior 128-byte-per-record figure is only a planning estimate and is
not a frozen storage format.

No new external flash is a current product requirement.

The current HISTORY_SECURE outer header exposes device_id, key_epoch,
security_counter and History incarnation, but an opaque gateway cannot verify
the inner AEAD tag. A malicious transmitter can therefore fabricate
structurally valid-looking frames. This is an availability/storage-wear threat,
not permission to weaken custody correctness.

Before gateway persistence is implemented on RAK4630/nRF52840:

- choose an explicit queue owner;
- define bounded ingress admission by enrolled/expected source plus per-source
  and global rate/capacity limits; visible source fields are useful for
  partitioning/rate limits but are not trusted authentication;
- ensure unauthenticated traffic cannot force unbounded flash writes or consume
  unbounded custody capacity;
- **never evict an ACKed-but-not-yet-durably-handed-off custody object merely
  to admit newer traffic**; once ACKed, that object is gateway-owned until
  durable Edge handoff or an explicit catastrophic-storage fault;
- preserve a fail-safe reserved-capacity/backpressure policy so queue pressure
  refuses new custody and produces no false ACK;
- review the existing application/geofence/security/config/bond/history layout;
- preserve DFU/bootloader/framework ownership;
- calculate firmware growth headroom separately from data-storage headroom;
- define power-cut commit/recovery;
- define erase/wear behavior;
- define queue-full behavior;
- budget reboot-time/full-scan reconstruction of the fingerprint/dedupe index,
  including bounded RAM and boot latency;
- include duplicate opaque objects caused by tracker reboot/checkpoint loss in
  capacity and wear calculations;
- prove that queue pressure never produces a false ACK.

Do not casually carve application flash into a custody partition.

---

## 9. RF, custody scope and power behavior

Tracker power remains the priority.

### 9.1 Initial custody scope

The first SF4 runtime applies custody to **HISTORY_SECURE History replay**, not
to legacy live TLP v1 POSITION packets. TLP v1 bytes/behavior remain unchanged.

SF3's current no-contact production probe cadence is deliberately conservative
and is not sufficient to drain a tracker producing multiple records per hour.
Therefore SF4D must add a separate **authenticated-custodian-present drain
mode**. Once trusted custody contact is established, backlog service capacity
for the product's supported cadence/topology must exceed new History production
with measured/modelled margin. When trusted contact disappears, behavior falls
back to the bounded low-rate probe policy.

No claim is made yet that every fresh position receives immediate custody, and
no faster runtime schedule is frozen in SF4A.

### 9.2 Quantitative gate

Before SF4C/D freezes ACK shape or scheduling, the model must quantify:

- HISTORY_SECURE uplink airtime;
- custody ACK airtime and batching;
- gateway half-duplex RX loss while transmitting ACKs;
- direct versus separately approved relay path;
- collision/retry/backoff assumptions;
- tracker post-TX RX-window timing;
- flash commit/readback latency before ACK;
- tracker record-production cadence;
- aggregate 10 / 30-50 / ~100-node collision-domain load;
- regional regulatory/channel constraints for the actual deployment profile.

The focused audit's current-profile sanity check
(SF11/BW125/CR4/5, direct, no retries) estimated a 73-byte HISTORY_SECURE frame
at about 1.724 s and an illustrative 32-byte ACK at about 0.987 s. At four
observations/hour, per-record ACK traffic is already a warning case near the
~100-node stress target. These are analytical warnings, not measured values,
not frozen ACK size, and not a regulatory conclusion.

The implementation gate is:

```text
confirmed custody drain rate
> new retained-record production rate
```

with an explicit engineering margin chosen and documented for every cadence and
topology the product claims.

SF4C may evaluate, rather than assume, mechanisms such as:

- bounded multi-object ACKs;
- delayed/aggregated ACK scheduling;
- Edge-connected fast handoff that minimizes gateway flash residence/wear;
- RF profile/channel/domain planning supported by the existing RF configuration
  architecture.

None is authorized merely by being listed here.

### 9.3 Flash-wear gate

SF4B must quantify bytes written, commit metadata, reclaim writes, page erases,
reboot duplicates and queue churn for the supported 10 / 30-50 / ~100-node
loads. A planning estimate of 128 bytes/object is not a storage-format freeze.

No gateway partition/queue size is accepted until the wear model demonstrates
product-life margin for the selected nRF52840 flash usage. If internal flash
cannot meet the requirement, SF4 must reduce custody write rate/residence or
move long-duration persistence to Edge rather than silently accepting a short
flash lifetime.

Custody ACK should fit the existing bounded post-TX receive/rendezvous strategy
where practical. SF4 must not quietly convert the tracker into an
always-listening receiver.

The first runtime slice is **direct Tracker -> Gateway custody only** unless a
separate relay timing/ownership review proves the relayed path; secure relay
forwarding is not silently activated by SF4.

The SF3 qualification image's 15-second replay interval remains test-only and is
not a production policy input.

---

## 10. Offline application boundary

SF4 custody does not define a second offline command/permission architecture.

Where local field operations later require protected user commands, they reuse
the reviewed delegated application authority direction in
`ORUN_TLP_V2_DELEGATED_COMMAND_SECURITY_DIRECTION.md`. Gateway custody only
defines observation responsibility transfer and durable synchronization.

The phone/Edge may remain useful without Internet for custody queueing and
already-authorized local application operations, but backend-owned account,
ownership, authority issuance/revocation and root-credential operations remain
backend/security-authority responsibilities.

No COMMAND/RESULT plaintext, delegation lifetime, mobile permission database or
sync API is frozen by this custody document.

---

## 11. Failure behavior

### Gateway hears frame but power fails before durable commit

No custody ACK is authoritative. Tracker retains/retries later.

### Gateway commits, ACK is lost

Tracker may replay the same protected custody object. Gateway dedupes and
re-ACKs. Gateway reboot must not make an already committed object impossible to
acknowledge: the final ACK-security design must retain/recover enough
authorization/counter state, or use an idempotent construction, so the gateway
can safely reissue custody proof after reset without weakening nonce/replay
rules.

### Gateway commits and ACK reaches tracker, then Internet disappears

Correct behavior: tracker does not resume that record merely because Internet
is absent. Gateway retains custody until Edge durable acceptance.

### Gateway buffer is full

No ACK for newly unretainable observations. Tracker remains responsible.
Already ACKed custody is never evicted merely to create room.

### Gateway commits, transfers to Edge, then reboots

Gateway may reclaim only if authenticated durable Edge acceptance bound to the
exact custody object was established. Ambiguous handoff keeps the safer copy.
The durable handoff evidence must be sufficient for reset recovery; a volatile
local-session success flag is not enough.

### Edge has data but no Internet

Edge retains the opaque custody object and synchronizes when Internet returns.
If the user-facing app separately has valid local authority/plaintext state, it
may present local/pending information; custody storage alone is not decryption
authority.

### Backend receives duplicate upload

Backend dedupes by logical observation identity while retaining useful path
metadata.

### Malicious/unauthorized ACK

Tracker rejects it with zero tracker-release mutation.

### Active RF attacker floods structurally valid-looking frames

Because the gateway intentionally remains opaque to the current inner
HISTORY_SECURE AEAD, it cannot cryptographically distinguish every forged
candidate before storage. Admission/rate/wear bounds must keep the attack
finite and must protect previously ACKed custody, but SF4 does **not** claim
availability against a sustained local RF attacker/jammer. Under pressure the
safe failure is refusal of new custody ACKs; tracker data remains retained and
replayed under its bounded policy.

### Gateway authorization is revoked while offline

Revocation has the same fundamental delayed-propagation problem as delegated
commands. The final custody authority design must bound residual offline
authority and define policy-floor/generation interaction rather than assuming
instant revocation.

---

## 12. Implementation slices

### SF4A — contract

This document plus updates to the canonical History/product architecture.
No runtime or wire changes.

### SF4B — gateway custody storage owner

Design and host-test the smallest power-cut-safe bounded queue with explicit
partition ownership and queue-full behavior.

### SF4C — authenticated custody ACK

Freeze the minimal security/wire contract after independent audit and test
vectors. Preserve TLP v1 bytes.

### SF4D — gateway runtime integration

HISTORY_SECURE RX -> durable queue -> custody ACK, with priority/backpressure
and no ACK-before-commit path.

### SF4E — tracker custody receipt integration

Accept authenticated custody ACK, stop replay, and advance coarse local release
state without per-record flash wear.

### SF4F — Edge handoff/synchronization contract

Define durable Gateway -> Edge and Edge -> Backend handoff semantics. Implement
phone/Pi/Linux adapters only after the shared contract is stable.

### SF5 — physical end-to-end qualification

At minimum:

```text
Tracker
 -> Gateway durable write
 -> authenticated custody ACK
 -> tracker replay stops
 -> gateway reset/recovery
 -> Edge durable handoff
 -> Internet outage
 -> backend synchronization
 -> duplicate/retry convergence
```

Physical PASS requires real devices/links. Host tests and RAK builds do not
substitute for this evidence.

---

## 13. Validation gates

Before production custody is claimed:

- prior TLP v1 compatibility/golden fixtures remain byte-identical;
- existing SF1-SF3 host/startup tests remain PASS;
- queue power-cut/torn-write recovery passes deterministic fault injection;
- ACK-before-durable-commit is impossible by construction/test;
- structurally valid unauthenticated RF flood is bounded in RAM/flash wear and
  **cannot evict already ACKed custody**; catastrophic storage failure is a
  separately diagnosed fault, not normal eviction policy;
- queue-full/fault emits no false ACK and leaves already ACKed custody intact;
- byte-identical protected retransmission/duplicate ACK behavior is idempotent;
- reboot-created opaque duplicates of one logical observation converge safely
  downstream;
- unauthorized/forged/replayed ACK causes zero tracker release mutation;
- custody-object identifier covers the entire exact protected frame and passes
  adversarial collision/substitution tests;
- gateway reset preserves every ACKed-but-not-yet-handed-off observation,
  rebuilds a bounded dedupe/fingerprint index within the SF4B boot/RAM budget,
  and permits safe re-ACK of committed objects;
- capacity pressure never evicts ACKed custody before durable Edge handoff;
- Edge reset preserves every durably accepted-but-not-backend-durable
  observation;
- authenticated Gateway -> Edge durable acceptance is object-bound and required
  before gateway reclaim;
- authenticated backend durable acceptance is required before Edge reclaim;
- backend accepts authenticated custody-delayed immutable HISTORY_SECURE
  observations outside the generic bounded D2A replay window and across
  decrypt-only retired epochs while custody remains unresolved;
- backend dedupe converges multiple gateway paths to one logical observation;
- no per-record tracker History metadata wear regression is introduced;
- RAK4630 build/partition guard passes;
- custody drain rate exceeds record-production rate with documented margin for
  every claimed cadence/topology;
- RF/airtime/load simulation and quantitative gateway flash-wear model pass for
  ~10, ~30-50 and ~100-device stress cases;
- physical two-device custody flow passes before the behavior is called
  physically qualified.

---

## 14. Explicit non-claims

This direction does not claim today that:

- gateway durable custody exists in production firmware;
- any current gateway may stop tracker replay merely by hearing a packet;
- an opaque gateway can see/decrypt HistoryRecordIdentity in the current
  HISTORY_SECURE payload;
- a custody ACK wire format or numeric context is frozen;
- gateway internal-flash queue size has been selected;
- 30-60 days of buffering belong on the RAK gateway;
- Edge/mobile/backend software is implemented;
- offline delegated application commands are implemented;
- SF3 single-device physical qualification proved gateway/backend behavior;
- visible HISTORY_SECURE header fields authenticate the claimed tracker to an
  opaque gateway;
- the first SF4 design prevents a sustained local RF attacker from jamming the
  channel or, absent a reviewed outer admission authenticator, from targeting a
  claimed source's admission quota.

The current physical evidence remains limited to SF3 device-side
HISTORY_SECURE origination/replay and the previously recorded production
storage/GNSS behavior.


## 15. Crypto implementation dependency

If SF4C/E selects a cryptographic full-frame fingerprint or standard MAC on the
RAK4630 path, production use is gated by the existing CC310/Bluefruit ownership
and readiness rules. Host vectors are insufficient to claim hardware/runtime
coexistence; use the already reviewed CryptoCell/Bluefruit validation boundary
rather than introducing a second crypto lifecycle owner.
