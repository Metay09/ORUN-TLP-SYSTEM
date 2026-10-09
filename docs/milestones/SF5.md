# SF5 — TLP v2 product observation + 128 KiB durable-data cutover

Status: **SF5A + SF5B + SF5C MERGED. SF5C IS PORTABLE STORAGE ONLY, NOT LINKED INTO PRODUCTION RUNTIME. SF5D PREFLIGHT IS OPEN (see `docs/milestones/SF5D.md`); no physical 128 KiB flash allocation, DFU-model clearance or SF5 production runtime is authorized.**

SF5A merge baseline:
`main@cdb9db172d178acc877b9315453e7adbd7947985`.

SF5B merged baseline:
`main@5b757966489e5fe221c05fe1cbf66ff7b1e602d5`.

Active SF5C branch:
`feat/sf5c-observation-store`.

SF5B frozen contract:
`docs/architecture/ORUN_TLP_V2_PRODUCT_SECURE_WIRE.md`.

SF5B milestone:
`docs/milestones/SF5B.md`.

SF5C milestone:
`docs/milestones/SF5C.md`.

Primary architecture contract:
`docs/architecture/ORUN_TLP_V2_PRODUCT_OBSERVATION_STORAGE_CUTOVER.md`.

Independent audit disposition:
`docs/audits/SF5A_TLP_V2_PRODUCT_OBSERVATION_AUDIT_DISPOSITION.md`.

## 1. Goal

Move the animal-tracker product from the legacy POSITION-only durable path to a
TLP v2 protected product-observation path while preserving current proven
storage/security/RF invariants until each explicit cutover gate is passed.

Target product path:

```text
sensor/source
  -> product observation
  -> 128 KiB tracker ObservationStore
  -> TLP v2 protected object
  -> Gateway durable custody
  -> Edge durable custody
  -> Backend
  -> App
```

Routine tracker data is one report-period observation:

```text
PERIODIC_OBSERVATION
  Location
  Activity summary for the same report period
  Battery / bounded telemetry
  Bounded health summary
```

Asynchronous durable tracker facts:

```text
EVENT
COMMAND_RESULT
```

MESSAGE is not part of the animal-tracker durable profile.

## 2. Owner-approved decisions already closed

- New production direction is TLP v2; TLP v1 remains frozen legacy compatibility.
- Tracker durable-data target is **128 KiB**.
- Gateway custody target is **128 KiB**.
- Owner decision 2026-10-08 supersedes the earlier SF5A 256 KiB planning target
  to preserve firmware-update/rollback headroom, including a future reviewed
  LoRa FOTA path. This is a storage-budget decision, not a claim that LoRa FOTA
  is implemented.
- Tracker full-store policy is oldest-first retention loss with explicit bounded
  capacity-loss diagnostics.
- Gateway accepted custody is never evicted for pressure; no durable commit means
  no custody ACK.
- Gateway -> Edge initial drain is global oldest-first/FIFO.
- Gateways are independent; duplicate custody is acceptable and downstream
  dedupe/idempotency is required.
- Activity summary covers the **effective report period actually selected by
  Tracking policy**; internal short sensor windows are not independently
  persisted.
- Battery is sampled only on an already-required device wake; routine battery
  checking never creates its own wake.
- EVENT is recorded on real occurrence/state transition; repeated phone
  reminders belong to app/backend.
- COMMAND side effects are idempotent; durable RESULT and replay/idempotency
  state are distinct.
- Product-affecting persistent health faults become EVENT; transient issues are
  bounded counters/status; raw debug is not routine durable product data.
- Direct cutover is preferred over permanent dual-write to legacy 28 KiB
  POSITION History.

## 3. Ordered slices

### SF5A — semantic product-data contract

Freeze only the application semantics and bounded field set for:

- PERIODIC_OBSERVATION;
- EVENT;
- COMMAND_RESULT.

Do not allocate packet/family numbers or freeze protected wire bytes yet.

Gate: focused architecture review; no production code change.

### SF5B — TLP v2 protected wire/security contract

Define exact application-family allocation, plaintext layout, protected-frame
binding, retry/reset protected-object lifetime and custody-ACK binding while
reusing the reviewed M7P6 security direction.

SF5B must explicitly define the transition from the **already active/frozen SF3
TLP v2 HISTORY_SECURE replay family**: the new SF5 protected product-observation
family replaces HISTORY_SECURE for new records at SF5F cutover; it must not
silently widen/reinterpret the existing 73-byte SF2/SF3 family.

Must preserve:

- gateway remains opaque;
- no tracker K_root on gateway/phone;
- nonce/replay/idempotency/delivery separation;
- exact-object custody binding;
- delayed immutable-observation old-epoch rules;
- COMMAND/RESULT delegated-security + CAS contracts;
- logical RESULT persistence is bounded per authenticated logical command, not
  per security-counter retry;
- offline read authority remains separable from tracker-origin forgery
  authority; do not freeze a wire/key binding that makes "read" necessarily
  mean "can forge PERIODIC/EVENT/RESULT".

SF5B is merged and independently reviewed. Its wire/security contract is now
the input to SF5C. No production codec/RF activation is implied by that merge.

Gate: **CLOSED for design**; implementation evidence belongs to later slices.

### SF5C — portable ObservationStore

Implement fixed-slot portable persistence over abstract FlashBackend:

- deterministic recovery;
- body/CRC then commit-last;
- bounded RAM and boot scan;
- oldest-first historical page rotation;
- explicit capacity-loss diagnostics;
- fixed record types PERIODIC / EVENT / RESULT;
- bounded **reset-safe tracker responsibility-release state**, including safe
  treatment of out-of-order custody acknowledgements;
- bounded proof that reset/checkpoint loss cannot re-protect/replay the whole
  backlog indefinitely;
- durable open-EVENT occurrence state that survives reset and historical ring
  eviction until CLEARED/reconciled;
- bounded RESULT retention/deduplication so authenticated retries do not append
  one persistent RESULT per security counter;
- no nRF address allocation yet.

Gate: complete host regression + ASan/UBSan/warnings + deterministic fault
injection/power-cut tests.

### SF5D — 128 KiB flash ownership + nRF backend

**First gate:** `docs/milestones/SF5D.md` records the read-only bootloader/DFU and hybrid Tracker+Gateway ownership preflight. The `[0x0C5000,0x0E5000)` range below is a CANDIDATE only. Do not lower the application ceiling or enable a physical writer until both gates close.


Candidate reference-platform range:

```text
0x0C5000..0x0E5000  128 KiB / 32 pages
```

Revise application ceiling only after current image size/headroom **and the DFU/
firmware-update model** are resolved. The candidate app/update planning range
`0x026000..0x0C5000` is 651,264 bytes; a simple equal two-bank arithmetic
split is about 325,632 bytes per bank. Against the current SF4B image measurement
of 285,924 bytes, that leaves about 39,708 bytes per equal bank.

This makes the current image arithmetically compatible with a two-image budget,
unlike the superseded 256 KiB target, but does not prove the actual bootloader
bank model or future growth headroom. Future LoRa FOTA remains a separate
security/power/RF/rollback design slice.

Wire the concrete nRF backend through the existing FlashMutationGate/
SoftDevice ownership model. No overlapping owner is allowed.

Before freezing the physical owner, resolve the real universal-firmware case
where one node originates its own observations **and** has Gateway bridge
capability. Do not solve this by legacy Role and do not let independent
ObservationStore/CustodyStore writers share pages.

Also freeze an explicit **owner-transition ceremony**. A profile/capability
change must not implicitly reformat a non-empty region. Unresolved accepted
Gateway custody blocks transition; tracker data may be destructively re-baselined
only through an explicit maintenance/data-loss decision after the old owner's
responsibility is closed or intentionally abandoned.

Gate: host suite + RAK4630 build + storage-layout guards.

### SF5E — physical ObservationStore qualification

On a development RAK4631 only:

- blank/preflight ownership proof;
- append/read/reset recovery;
- page rotation;
- real power-cut/torn-write boundaries where safely testable;
- verify Geofence/Security/Config/BLE/legacy History/bootloader ranges remain
  untouched.

Gate: physical evidence document. Build PASS alone is not hardware PASS.

### SF5F — tracker runtime cutover

Move normal tracker durable ownership from legacy POSITION History to the new
ObservationStore:

- one PERIODIC record per **effective** report period;
- EVENT/RESULT asynchronous records;
- no permanent dual-write;
- no routine raw accelerometer/GNSS/debug persistence;
- the new SF5 protected family becomes the normal product store-forward path
  only after SF5B/C/D/E gates are satisfied;
- retire the legacy SF3 HISTORY_SECURE replay runtime together with the legacy
  HistoryStore for new records; do not reinterpret its frozen bytes.

Before SF5F, the current runtime remains: frozen TLP v1 live POSITION/relay plus
provisioned SF3 TLP v2 HISTORY_SECURE replay. Legacy v1 fixtures **and SF2/SF3
HISTORY_SECURE vectors/bytes** remain frozen and must not be weakened.

Gate: full host + target build + required physical tracking/GNSS/power/storage
regression + independent final audit.

### SF5G — Gateway custody runtime closure

Close SF4B pre-runtime gates N1-N5, select/wire the 128 KiB Gateway custody
owner for the relevant product profile, then enable authenticated
GATEWAY_CUSTODY_ACK only after a reviewed durable handoff fact.

Before enabling the SF5 connected-path write-around branch, measure and freeze a
timing bound such that:

```text
Edge wait budget
+ worst-case local fallback commit/readback
+ custody ACK airtime
<= tracker ACK rendezvous window
```

If the bound does not fit for the active Edge transport/RF profile, the Gateway
must skip Edge waiting and take the local durable-custody path immediately for
that attempt. Write-around must not increase tracker retransmissions by missing
the ACK rendezvous window.

The existing SF4B CustodyStore format is frozen around the current 73-byte
HISTORY_SECURE object. If SF5B produces a different protected-object size, do
not silently stretch that format or reuse its 43-record/page capacity numbers.
Version/review the Gateway custody format and rerun power-cut/recovery/wear/scan
analysis for the actual SF5 object size.

Gate: host + target + physical custody qualification + independent audit.

### SF5H — Edge sync / offline local data

Implement Gateway -> Edge durable handoff and FIFO drain using the first chosen
local transport. Edge must durably accept before Gateway reclaim.

Offline phone/Edge decryption requires a separately reviewed bounded/scoped
offline-read authority; do not copy K_root to phone. The chosen SF5B envelope
must leave a path to grant read/decrypt access without also granting authority
to forge tracker-originated PERIODIC/EVENT/RESULT. If the selected symmetric
construction cannot provide that property, it must be an explicit reviewed
security limitation before the wire contract is frozen.

Gate: Tracker -> Gateway -> Edge end-to-end offline test.

### SF5I — Backend/app completion

Backend ingestion/dedupe, gap visibility, freshness semantics, event lifecycle,
command-result lifecycle and app presentation.

End-to-end final gate:

```text
Tracker -> Gateway -> Edge -> Backend -> App
```

with Internet/RF/power/reset interruption scenarios.

## 4. Validation discipline

For runtime/RF/security/persistence/power changes:

```text
host tests
-> warnings / sanitizers
-> RAK4630 build
-> physical test when required
-> independent audit
-> fixes
-> repeat affected gates
-> merge
```

Documentation-only slices may use a lighter gate, but must not claim runtime or
physical evidence.

## 5. Do not combine into one rewrite

SF5 deliberately keeps semantic contract, protected wire, portable persistence,
physical layout, runtime cutover and Edge/backend work as separate reviewable
steps.

A later slice may refine this ordering only with an explicit reason; it may not
silently bypass security, power-cut, compatibility or physical-evidence gates.
