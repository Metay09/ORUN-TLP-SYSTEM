# ORUN Gateway Durable Custody / Edge Synchronization Direction

Status: **OWNER-APPROVED SF4A DESIGN DIRECTION — CONTRACT UNDER REVIEW; NO NEW WIRE BYTES, FLASH PARTITION OR PRODUCTION RUNTIME AUTHORIZED YET.**

Baseline: `main@572f92cdb664150ac0dfec8394573da71061d1bd`.

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
> RF replay of the explicitly named logical observation, but only after the
> gateway has committed that observation to its own reviewed persistent queue.

This intentionally changes the earlier first-slice rule that only
BACKEND_DURABLE could release tracker History. The old rule remains historical
audit evidence for SF0-SF3; this SF4A amendment requires its own independent
security/storage review before production implementation.

---

## 4. Chain-of-custody invariants

### 4.1 Tracker -> Gateway

The gateway may emit GATEWAY_CUSTODY_ACK only after all of these are true:

1. the received HISTORY_SECURE frame is structurally admissible for custody;
2. the gateway has enough durable queue capacity;
3. the complete custody record has been committed using the reviewed
   power-cut-safe storage protocol;
4. the committed record can be recovered as authoritative after reset;
5. the ACK can be authenticated as coming from an authorized custody-capable
   gateway.

The gateway must **not ACK** when the queue is full, faulted, busy beyond the
admission bound, or the durable write outcome is unknown.

Once the tracker accepts a valid custody ACK for an explicit logical History
identity:

- it stops RF replay of that observation;
- it may advance its local releasable/delivered prefix according to the
  reviewed coarse History checkpoint policy;
- it does not wait for Internet or BACKEND_DURABLE before considering the RF
  custody responsibility transferred.

Physical History page erasure remains capacity-driven. Custody ACK must not
introduce per-record flash metadata writes or weaken the existing
metadata-only-erase prohibition.

### 4.2 Gateway -> Edge

The gateway retains its custodied record until an Edge durably accepts the same
logical observation.

Only after durable Edge acceptance may the gateway reclaim the corresponding
queue entry.

A volatile BLE/USB/UART transfer, process RAM enqueue or socket write is not
enough.

### 4.3 Edge -> Backend

The Edge retains its record while Internet/backend is unavailable.

Only after backend durable acceptance (or idempotent recognition of the same
logical observation) may the Edge reclaim the local queue entry.

Internet restoration triggers synchronization; it is not required for local
field collection.

---

## 5. Stable identity and dedupe

The existing logical History observation identity remains:

```text
(DeviceIdentity, HistoryIncarnation, HistoryRecordIdentity)
```

Custody changes ownership of responsibility, not observation identity.

Therefore:

- the same observation through two gateways remains one logical observation;
- duplicate gateway custody attempts are idempotent;
- duplicate Edge/backend uploads are idempotent;
- gateway/edge/backend may retain path metadata separately;
- a retry or different secure transport counter must never mint another
  application observation.

A custody ACK must identify the exact logical observation. The final wire may
also bind a frame fingerprint if required by the security review, but no hash or
wire field is frozen by this document.

---

## 6. Security boundary

A forged custody ACK could make a tracker stop replaying data that no real
custodian holds. Custody ACK is therefore security-sensitive.

Required properties:

- authenticated origin;
- authorization specifically for **gateway custody**;
- target tracker binding;
- History incarnation binding;
- explicit History record identity binding;
- replay/duplication safety;
- revocation/re-enrollment behavior;
- no unauthenticated TLP v1 fallback.

Custody authority is **not** BACKEND_DURABLE authority and is **not**
application COMMAND authority.

The gateway must not receive tracker `K_root`.

The existing delegated-gateway security architecture is a candidate foundation,
but SF4A does not silently reuse `DELEGATED_GW2D`, `K_grant`, command scopes,
or numeric security-context values. A custody-specific authorization/key/context
must be independently reviewed before bytes are frozen.

No fleet-wide custody secret is introduced.

---

## 7. Multi-gateway behavior

A tracker is not locked to one gateway.

Any currently authorized custody-capable gateway may accept an observation.

If multiple gateways durably store the same observation:

- each may independently hold a duplicate custody copy;
- the tracker may accept the first valid custody ACK;
- downstream dedupe uses the stable logical observation identity;
- no gateway obtains exclusive ownership of the tracker.

If an ACK is lost, the tracker may replay later. A gateway that already holds
the observation must treat the duplicate idempotently and may reissue an
authenticated ACK without creating a second logical queue item.

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

Before gateway persistence is implemented on RAK4630/nRF52840:

- choose an explicit queue owner;
- review the existing application/geofence/security/config/bond/history layout;
- preserve DFU/bootloader/framework ownership;
- calculate firmware growth headroom separately from data-storage headroom;
- define power-cut commit/recovery;
- define erase/wear behavior;
- define queue-full behavior;
- prove that queue pressure never produces a false ACK.

Do not casually carve application flash into a custody partition.

---

## 9. RF and power behavior

Tracker power remains the priority.

Custody ACK should fit the existing bounded post-TX receive/rendezvous strategy
where practical. SF4 must not quietly convert the tracker into an always-listening
receiver.

Exact ACK turnaround, direct/relay path behavior and retry timing remain
unfrozen until modeled and measured.

Required policy direction:

- live/critical work outranks backlog;
- successful custody stops redundant tracker backlog RF;
- no gateway/Internet absence creates a tight replay loop;
- gateway queue-full/fault falls back to tracker retention and bounded retry;
- 10 / 30-50 / ~100-device RF-domain load must be simulated before claiming
  capacity.

The SF3 qualification image's 15-second replay interval remains test-only and is
not a production policy input.

---

## 10. Offline-first application / authorization direction

Local product operation should not require Internet when the user already has
valid authority.

The intended behavior is:

```text
authorized user + valid local delegated authority
  -> App/Edge -> Gateway -> Tracker
  -> local durable result/state
  -> synchronize with Backend when Internet returns
```

Online and offline operation use one authorization model, not two unrelated
permission systems.

The phone/Edge may cache bounded, revocable delegated authority suitable for
ordinary field operations. Backend-owned operations such as account ownership,
authority issuance/revocation, credential-root changes or ownership transfer
remain backend/security-authority responsibilities.

This section records product direction only. It does not freeze COMMAND/RESULT
plaintext, delegation lifetime, mobile storage schema or synchronization API.

---

## 11. Failure behavior

### Gateway hears frame but power fails before durable commit

No custody ACK is authoritative. Tracker retains/retries later.

### Gateway commits, ACK is lost

Tracker may replay. Gateway dedupes and re-ACKs.

### Gateway commits and ACK reaches tracker, then Internet disappears

Correct behavior: tracker does not resume that record merely because Internet
is absent. Gateway retains custody until Edge durable acceptance.

### Gateway buffer is full

No ACK for newly unretainable observations. Tracker remains responsible.

### Gateway commits, transfers to Edge, then reboots

Gateway may reclaim only if durable Edge acceptance was itself established by
the reviewed handoff protocol. Ambiguous handoff keeps the safer copy.

### Edge has data but no Internet

Edge retains it and presents appropriate local/pending state. It synchronizes
when Internet returns.

### Backend receives duplicate upload

Backend dedupes by logical observation identity while retaining useful path
metadata.

### Malicious/unauthorized ACK

Tracker rejects it with zero delivery-state mutation.

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
- queue-full/fault emits no false ACK;
- duplicate frame/duplicate ACK behavior is idempotent;
- unauthorized/forged/replayed ACK causes zero tracker release mutation;
- gateway reset preserves every ACKed-but-not-yet-handed-off observation;
- Edge reset preserves every durably accepted-but-not-backend-durable
  observation;
- backend dedupe converges multiple gateway paths to one logical observation;
- no per-record tracker History metadata wear regression is introduced;
- RAK4630 build/partition guard passes;
- RF/airtime/load simulation passes for ~10, ~30-50 and ~100-device stress
  cases;
- physical two-device custody flow passes before the behavior is called
  physically qualified.

---

## 14. Explicit non-claims

This direction does not claim today that:

- gateway durable custody exists in production firmware;
- any current gateway may stop tracker replay merely by hearing a packet;
- a custody ACK wire format or numeric context is frozen;
- gateway internal-flash queue size has been selected;
- 30-60 days of buffering belong on the RAK gateway;
- Edge/mobile/backend software is implemented;
- offline delegated application commands are implemented;
- SF3 single-device physical qualification proved gateway/backend behavior.

The current physical evidence remains limited to SF3 device-side
HISTORY_SECURE origination/replay and the previously recorded production
storage/GNSS behavior.
