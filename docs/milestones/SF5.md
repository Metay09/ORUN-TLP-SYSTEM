# SF5 — TLP v2 product observation + 256 KiB durable-data cutover

Status: **IN PROGRESS — product direction captured; implementation not yet authorized.**

Baseline: `main@89492c8d87b42fe1c9bd6bb334b41812640db74b` (SF4B merged).

Active branch: `design/tlp-v2-product-observation-cutover`.

Primary architecture contract:
`docs/architecture/ORUN_TLP_V2_PRODUCT_OBSERVATION_STORAGE_CUTOVER.md`.

## 1. Goal

Move the animal-tracker product from the legacy POSITION-only durable path to a
TLP v2 protected product-observation path while preserving current proven
storage/security/RF invariants until each explicit cutover gate is passed.

Target product path:

```text
sensor/source
  -> product observation
  -> 256 KiB tracker ObservationStore
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
- Tracker durable-data target is 256 KiB.
- Gateway custody target is 256 KiB.
- Tracker full-store policy is oldest-first retention loss with explicit bounded
  capacity-loss diagnostics.
- Gateway accepted custody is never evicted for pressure; no durable commit means
  no custody ACK.
- Gateway -> Edge initial drain is global oldest-first/FIFO.
- Gateways are independent; duplicate custody is acceptable and downstream
  dedupe/idempotency is required.
- Activity summary covers the configured report period; internal short sensor
  windows are not independently persisted.
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

Must preserve:

- gateway remains opaque;
- no tracker K_root on gateway/phone;
- nonce/replay/idempotency/delivery separation;
- exact-object custody binding;
- delayed immutable-observation old-epoch rules;
- COMMAND/RESULT delegated-security + CAS contracts.

Gate: independent security/protocol audit before runtime activation.

### SF5C — portable ObservationStore

Implement fixed-slot portable persistence over abstract FlashBackend:

- deterministic recovery;
- body/CRC then commit-last;
- bounded RAM and boot scan;
- oldest-first page rotation;
- explicit capacity-loss diagnostics;
- fixed record types PERIODIC / EVENT / RESULT;
- no nRF address allocation yet.

Gate: complete host regression + ASan/UBSan/warnings + deterministic fault
injection/power-cut tests.

### SF5D — 256 KiB flash ownership + nRF backend

Candidate reference-platform range:

```text
0x0A5000..0x0E5000  256 KiB / 64 pages
```

Revise application ceiling only after current image size/headroom is checked.
Wire the concrete nRF backend through the existing FlashMutationGate/
SoftDevice ownership model. No overlapping owner is allowed.

Before freezing the physical owner, resolve the real universal-firmware case
where one node originates its own observations **and** has Gateway bridge
capability. Do not solve this by legacy Role and do not let independent
ObservationStore/CustodyStore writers share pages.

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

- one PERIODIC record per report period;
- EVENT/RESULT asynchronous records;
- no permanent dual-write;
- no routine raw accelerometer/GNSS/debug persistence;
- v2 protected TX path becomes the normal product path only after SF5B/C/D/E
  gates are satisfied.

Legacy v1 fixtures remain frozen and must not be weakened.

Gate: full host + target build + required physical tracking/GNSS/power/storage
regression + independent final audit.

### SF5G — Gateway custody runtime closure

Close SF4B pre-runtime gates N1-N5, select/wire the 256 KiB Gateway custody
owner for the relevant product profile, then enable authenticated
GATEWAY_CUSTODY_ACK only after durable commit.

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
offline-read authority; do not copy K_root to phone.

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
