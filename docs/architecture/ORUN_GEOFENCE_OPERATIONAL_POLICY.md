# ORUN Geofence Operational Policy

Status: **OWNER-APPROVED DESIGN DIRECTION; M6D1 PURE STATE OWNER IMPLEMENTED IN PR #51 BUT NOT PRODUCTION-RUNTIME INTEGRATED — 2026-09-27**

This document defines the current M6D geofence operational-state baseline. It
does not claim runtime integration, field validation, a new TLP packet, backend
implementation or trustworthy LOST/contact semantics.

It refines the earlier conceptual geofence defaults in `AGENTS.md` and M6
planning. Historical M6C1/M6C2/M6C3 milestone evidence remains valid for what it
actually proved: geometry, permitted-area composition and bounded runtime
ownership. This document governs the later operational policy built on top of
those foundations.

## 1. Ownership and non-goals

Geofence truth is decided locally on the TRACKER from already accepted fresh
Location observations.

Keep these responsibilities separate:

```text
Location acceptance / quality
!= Geofence geometry
!= Geofence operational state
!= Tracking cadence
!= Event transport
!= Backend notification
!= LOST/contact
```

The geofence owner must not read raw GNSS-driver internals. The current reference
TRACKER obtains Location from GNSS, but Geofence consumes accepted Location.

This policy does not:

- change TLP v1 bytes or add an unauthenticated v1 alarm packet;
- make `TX_DONE` mean delivery, contact or alarm receipt;
- define trustworthy LOST;
- persist transient effective cadence in ConfigStore;
- add a user-visible `NEAR_FENCE` state;
- authorize backend reclassification of POSITION packets as the source of the
  geofence alarm decision.

## 2. Base and effective tracking intervals

Let:

```text
B = configured base tracking interval
```

`B` is durable requested configuration owned by the normal configuration
path.

The geofence policy may derive a runtime effective interval:

```text
INSIDE  -> effective interval = B
OUTSIDE -> effective interval = B / 3
```

`B / 3` is runtime policy only. It must not overwrite `B` in ConfigStore.

The exact integer rounding/minimum-bound rule is an M6D implementation detail
that must respect the existing GNSS scheduling safety contract. A policy change
must never create overlapping acquisitions or catch-up bursts.

## 3. Current operational states

The current product baseline exposes only:

```text
INSIDE
OUTSIDE
```

`NEAR_FENCE` is not part of the current M6D operational policy. It remains a
possible future product capability only if field evidence shows it is useful.

The existing geometry layer may still return `BOUNDARY`. That is a geometry
fact, not a third product state.

The first M6D implementation freezes BOUNDARY handling as follows:

- BOUNDARY by itself never starts a state-transition confirmation episode;
- during an already-started three-observation episode, BOUNDARY consumes one of
  the bounded evidence slots but contributes neither an INSIDE nor OUTSIDE vote;
- if three consumed evidence slots do not produce the required 2-of-3 transition
  majority, the previous confirmed state remains authoritative;
- while internally unclassified after boot/config replacement, BOUNDARY does
  not establish a product state.

This prevents an edge-hugging position sequence from creating either an
unbounded confirmation burst or a false OUTSIDE transition.

## 4. Transition confirmation

Extra GNSS/Location confirmation is paid only when the apparent state changes.

### 4.1 INSIDE -> INSIDE

No extra confirmation.

- store/transmit the normal accepted position through the existing
  store-before-send path;
- keep effective interval `B`.

### 4.2 INSIDE -> OUTSIDE candidate

A single OUTSIDE observation is not sufficient for a state transition or alarm
event.

The first accepted OUTSIDE observation starts one bounded confirmation episode:

- the first candidate counts as fix/observation #1;
- request at most two additional accepted fresh observations;
- maximum total evidence is three accepted observations;
- use a 2-of-3 majority to decide the transition;
- do not run a second overlapping acquisition owner;
- do not use stale coordinates as confirmation.

The intended field behavior is to obtain the complete three-observation set
before declaring a transition, including when the first two observations are
both OUTSIDE. This is deliberate false-alarm protection.

If the bounded confirmation episode cannot obtain enough accepted observations
to reach the required decision, it must fail safe: no new OUTSIDE state/event is
invented from insufficient evidence. The exact confirmation time budget and
retry timing are M6D implementation parameters and must be physically validated.

If the completed majority is OUTSIDE:

- transition to `OUTSIDE`;
- select a real accepted observation from the OUTSIDE majority for the logical
  position report; do not synthesize an averaged coordinate;
- store it through HistoryStore before live transmission;
- transmit the POSITION immediately when transport admission allows; do not wait
  for the next tracking interval;
- emit one local OUTSIDE event occurrence;
- set effective tracking interval to `B / 3`.

The exact representative-fix quality tie-break is not frozen by this policy and
must use existing accepted-location quality information rather than inventing a
new location.

If the completed majority is INSIDE:

- remain `INSIDE`;
- emit no OUTSIDE event;
- keep effective interval `B`.

### 4.3 OUTSIDE -> OUTSIDE

No extra confirmation.

- send/store the normal accepted POSITION;
- remain `OUTSIDE`;
- keep effective interval `B / 3`;
- do not emit another OUTSIDE occurrence merely because another scheduled
  position is still outside.

This prevents continuous three-fix bursts and repeated alarms while the animal
remains outside.

### 4.4 OUTSIDE -> INSIDE candidate

Use the same bounded transition-confirmation principle in the reverse direction:

- at most three accepted observations total;
- 2-of-3 majority;
- majority INSIDE -> transition to `INSIDE` and restore effective interval
  `B`;
- majority OUTSIDE -> remain `OUTSIDE` and keep `B / 3`.

Return-to-inside event/notification wire semantics are not frozen here. They
belong to the later Event lifecycle design.

## 5. POSITION versus OUTSIDE event

POSITION and geofence event are different application facts.

A true confirmed INSIDE -> OUTSIDE transition produces:

1. the selected real POSITION through the normal store-before-send position
   path; and
2. one OUTSIDE event occurrence owned by the local geofence/event service.

The backend must not add a second majority/waiting layer before warning the
user. When it receives an authenticated/trusted TRACKER OUTSIDE event, it may
raise the user-visible geofence warning.

The backend must not treat an ordinary POSITION packet as an independent
authoritative reason to manufacture the same tracker geofence alarm. It may
store/render positions and perform separate diagnostics/analytics, but the
device-generated OUTSIDE event is the source of this alarm state.

Current TLP v1 has no approved secure OUTSIDE event packet. Therefore the
semantic event may be implemented locally before its RF transport exists, but
production remote delivery must use the reviewed secure EVENT/v2 path rather
than an unauthenticated v1 shortcut.

Geofence violation is critical traffic. When its secure transport is later
implemented it must follow the existing critical-event requirements: stable
event identity, duplicate safety, bounded retry/ACK/custody semantics and
store-forward behavior appropriate to the threat model. Local `TX_DONE` is
not delivery evidence.

## 6. Active-area-set replacement / clear boundary

Operational INSIDE/OUTSIDE evidence is meaningful only for the exact active
geofence resource against which it was observed.

When an authorized production config owner atomically replaces the active area
set:

- cancel any in-progress transition-confirmation episode from the old set;
- invalidate the old operational classification internally without inventing a
  third user-visible geofence state;
- emit no synthetic INSIDE/OUTSIDE transition event solely because the
  configuration changed;
- use configured base cadence `B` while the new set has not yet been
  reclassified from fresh accepted Location evidence;
- evaluate the next accepted fresh Location against the new set;
- if the new evidence indicates OUTSIDE, use the normal bounded confirmation
  episode before entering confirmed OUTSIDE and switching to `B / 3`.

When an authorized CLEAR removes/disables the geofence resource:

- cancel any in-progress confirmation episode;
- stop geofence evaluation;
- restore runtime cadence to configured base `B`;
- emit no synthetic INSIDE/return event solely because the fence was removed.

CLEAR is not FREE_GRAZE. FREE_GRAZE remains a separate operational policy.

If an authenticated REPLACE/CLEAR occurs while the backend has an open OUTSIDE
alarm, config success must not masquerade as a physical return-to-INSIDE event.
The later Event/backend lifecycle contract must define an authenticated
"superseded/cleared by configuration change" resolution path so an old alarm does
not remain permanently open merely because no synthetic INSIDE event is emitted.

Production geometry/configuration ownership is defined separately in
`ORUN_GEOFENCE_CONFIGURATION_DISTRIBUTION.md`. M6D host/physical development
may use explicit test-only area-set fixtures; those fixtures are not production
configuration.

## 7. Reboot and persistence boundary

`B` remains durable configuration.

`B / 3` is transient effective state and is not persisted as a replacement
configuration value.

The first M6D implementation does **not** persist confirmed geofence operational
state across reboot.

After boot/reset:

- internal operational state starts unclassified;
- runtime cadence policy is base `B`;
- BOUNDARY does not establish a state;
- a fresh accepted INSIDE observation establishes INSIDE;
- a fresh accepted OUTSIDE observation starts the normal full three-observation
  confirmation before OUTSIDE may become authoritative.

When that post-boot/config-replacement confirmation establishes OUTSIDE, M6D1
reports **initialized OUTSIDE**, not a physical INSIDE -> OUTSIDE transition.
The later Event lifecycle must decide how to reconcile an initialized OUTSIDE
state with any pre-existing/open backend alarm; it must not infer a new physical
crossing solely from reboot or configuration replacement.

No geofence operational-state bytes are added to ConfigStore, HistoryStore or
another flash owner in this slice. A later persistence proposal, if field/product
evidence justifies one, requires its own storage/power-cut review.

## 8. LOST remains separate

`OUTSIDE` does not imply `LOST`.

The existing security boundary remains unchanged:

```text
OUTSIDE + no network contact for N hours
!= trustworthy LOST
```

until "network contact" is backed by an appropriate authenticated
receipt/contact semantic.

The local OUTSIDE state and its `B / 3` tracking cadence may exist before
trustworthy LOST/contact is implemented.

## 9. Implementation gate

Before M6D production runtime integration is merged, the remaining implementation
slice must explicitly close:

- exact confirmation timing/deadline;
- representative-fix tie-break;
- runtime schedule re-anchoring when `B <-> B/3` changes;
- behavior when confirmation acquisition times out at the GNSS/acquisition owner
  (the pure state machine already defines abort as no-transition/fail-safe);
- OUTSIDE event identity and secure transport dependency;
- host tests for every transition and no-overlap invariant;
- RAK4630 build;
- focused physical GNSS/geofence validation on a GNSS-equipped unit.

Do not weaken M6C geometry fixtures, TLP v1 golden fixtures, GNSS no-overlap
rules or the existing store-before-send invariant to make this policy pass.
