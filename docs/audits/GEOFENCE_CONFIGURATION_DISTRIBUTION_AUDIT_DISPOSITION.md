# Geofence Configuration / Distribution — Independent Audit Disposition

Status: **INITIAL INDEPENDENT AUDIT PASS WITH FIXES; 0 BLOCKER / 1 HIGH / 4 MEDIUM / 6 LOW; REQUIRED H1/M1-M4 + RELEVANT LOW FIXES APPLIED; FINAL FOCUSED VERIFY PENDING — 2026-09-27**

PR: **#50 — Docs: define geofence configuration and fleet distribution direction**

Base: `main@c9cfc30344e7c5dfbcf9ccc8d6577af578ef068c`

Initial reviewed head:
`aafabecd2f3a292081607f37e2fda0cab17ab47b`

This audit is architecture/design review only. PR #50 changes documentation and
does not change production firmware, TLP v1 bytes, RF runtime, GNSS runtime,
storage allocation or BLE GATT bytes.

---

## Verdict

Initial independent verdict:

**PASS WITH FIXES**

- BLOCKER: 0
- HIGH: 1
- MEDIUM: 4
- LOW: 6

The direction/ownership model was accepted. The required findings concerned
airtime/resource-transfer feasibility, M6D sequencing, staging-vs-CAS ownership,
CLEAR/empty/state semantics and RESULT truth.

---

## H1 — bulk LoRa geofence transfer / fleet airtime

### Finding

The initial draft required per-device fan-out and airtime pacing without
quantifying the existing delegated-command payload/counter constraints.

The current delegated candidate has:

- 32-byte protected plaintext ceiling;
- 24-byte COMMAND fixed portion;
- 8 bytes remaining for ordinary args;
- one distinct outstanding delegated frame per tracker until authenticated
  RESULT or the safe uncertainty deadline.

A 64-vertex polygon is already 512 raw coordinate bytes before metadata.

### Disposition

**ACCEPTED / FIX APPLIED.**

The corrected direction now:

- records ~2.134 s airtime for a 96-byte SF11/BW125/CR4/5 frame as an engineering
  illustration rather than a regulatory hardcode;
- records the rough 16..64 frame/body range and ~10/~40/~100 tracker direct-airtime
  warning arithmetic;
- states explicitly that geofence bulk transfer must not reuse the ordinary
  small-config COMMAND family;
- requires a separately reviewed protected resource-transfer design reconciled
  with delegated counter/HWM and single-outstanding-frame rules;
- requires a small authenticated resource-state/content-digest preflight before
  bulk LoRa transfer;
- makes BLE the preferred bulk path when local access is available;
- requires secure content reuse/caching or equivalent reviewed optimization to be
  evaluated before large identical fleet rollout when measured airtime warrants
  it.

No secure multicast/group-auth key is authorized by this correction.

---

## M1 — M6D must not be blocked on M7/TLP v2

### Finding

The first draft's implementation order could be read as requiring production
durable/remote configuration before any M6D runtime work, conflicting with M6's
local scope.

### Disposition

**ACCEPTED / FIX APPLIED.**

M6D local operational behavior may proceed first against a narrow read-only
active-area-set provider seam. Host tests and focused physical GNSS/geofence
validation may use an explicit compile-gated/test-only fixture fence.

Only the production durable/user-mutable geofence source and BLE/LoRa mutation
paths remain gated on later storage/security work. A test-only fixture is not a
fake production fence.

---

## M2 — fragment staging must not hold the mutation slot

### Finding

The initial wording serialized authorization -> fragment transfer -> CAS ->
validation -> commit as one transaction. A long LoRa transfer could therefore
block local BLE/recovery for hours.

### Disposition

**ACCEPTED / FIX APPLIED.**

The corrected contract separates:

1. non-authoritative, bounded, cancellable candidate fragment staging; and
2. a short serialized semantic commit phase after complete content/integrity
   verification.

CAS/equality evaluation and the semantic mutation slot occur only in the short
commit phase.

A locally authorized BLE operator may abandon an incomplete remote staging
candidate before commit, subject to the future power-cut-safe storage contract.
No transport may preempt an already-admitted semantic commit.

---

## M3 — empty/CLEAR semantics and M6D state interaction

### Finding

The initial draft allowed "enabled + zero polygons", did not separate CLEAR from
FREE_GRAZE, did not assign the enable bit to the same resource owner, and did
not define the effect of REPLACE/CLEAR on confirmed OUTSIDE/B/3 state or an
active confirmation episode.

### Disposition

**ACCEPTED / FIX APPLIED.**

The corrected direction now requires:

- enabled geofence => one or more valid polygons;
- enabled + zero polygons => reject;
- enabled/disabled state owned by the same geofence resource as the geometry;
- CLEAR_SET => no active geofence/service disabled;
- CLEAR_SET != FREE_GRAZE;
- REPLACE cancels old confirmation, invalidates old classification, emits no
  synthetic transition, uses B while unclassified, then evaluates fresh
  Location and still requires normal bounded confirmation before new OUTSIDE;
- CLEAR cancels confirmation/evaluation, restores B and emits no synthetic
  return/INSIDE event.

The operational policy is updated with the same boundary.

---

## M4 — per-device RESULT truth / delegated family registration

### Finding

The initial bulk-result taxonomy omitted UNCONFIRMED and did not constrain
DELIVERED_TO_DEVICE/FAILED to authenticated evidence. It also implied gateway
store-forward before geofence was explicitly admitted to the delegated
opcode/scope/freshness family.

### Disposition

**ACCEPTED / FIX APPLIED.**

The corrected direction now:

- adds UNCONFIRMED;
- requires authenticated device evidence before DELIVERED_TO_DEVICE;
- requires authenticated RESULT/reconciliation for APPLIED and definitive
  application failure/rejection states;
- preserves transport timeout without RESULT as UNCONFIRMED, never FAILED;
- separates OFFLINE/UNREACHABLE from an already-possible UNCONFIRMED outcome;
- requires explicit future opcode -> scope registry entries for geofence
  resource read/state, REPLACE and CLEAR;
- requires CLEAR authorization to represent its alarm-protection impact;
- explicitly states that current delegated §12.1 does not authorize an ad-hoc
  multi-frame geofence blob;
- requires the resource-transfer family to be separately admitted to delayed
  store-forward policy before gateway custody is allowed.

The delegated-command architecture document is updated accordingly without
freezing numeric opcode/scope values.

---

## LOW findings

Relevant LOW items were folded into the same correction because they improve
canonical consistency without expanding implementation scope:

- complete M6C geometry validation is required, not vertex count only;
- multi-polygon semantics are explicitly the existing permitted union; overlap is
  allowed, no holes/exclusion semantics are invented, order has no product
  meaning;
- BLE bonding remains insufficient authorization and tracker user/phone ACL is
  still prohibited; exact local application authority remains a later reviewed
  design;
- persistence review must include bootloader/application/DFU dual-bank budget;
- this disposition file records the audit chain rather than labeling the
  unaudited draft as final.

Commit-history squashing is repository hygiene, not an architecture requirement;
it may be handled by the eventual PR merge method.

---

## Remaining gates

This disposition does **not** authorize implementation.

Still unfrozen / required later:

- total area/vertex/snapshot capacity;
- exact geofence persistence owner/partition/layout;
- exact geofence CAS-token scope;
- exact protected resource-transfer framing;
- exact BLE application authorization;
- exact numeric opcode/scope registry;
- actual RF airtime/load results for the eventual frame design;
- backend/mobile schema/UI;
- secure OUTSIDE EVENT delivery.

No new physical test is required for these documentation corrections.

A focused independent final verification should confirm H1 and M1-M4 are closed
and that no new contradiction with M6, delegated-command security, CAS or
current geometry semantics was introduced.
