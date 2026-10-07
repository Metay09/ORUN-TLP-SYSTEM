# ORUN TLP v2 Tracker Product-Data Semantic Contract

Status: **SF5A CANDIDATE — semantic/application contract only. NO WIRE NUMBERS,
BYTE LAYOUT, FLASH SLOT SIZE OR PRODUCTION RUNTIME IS FROZEN HERE.**

Baseline: `main@89492c8d87b42fe1c9bd6bb334b41812640db74b`.

Parent direction:
`docs/architecture/ORUN_TLP_V2_PRODUCT_OBSERVATION_STORAGE_CUTOVER.md`.

## 1. Purpose

Define the smallest bounded product facts the animal tracker must durably own
before SF5B assigns protected TLP v2 wire bytes and SF5C assigns on-flash slot
bytes.

The semantic record families are exactly:

```text
PERIODIC_OBSERVATION
EVENT
COMMAND_RESULT
```

Human MESSAGE is intentionally absent from the animal-tracker profile.

The contract stores product meaning, not driver internals and not a transport
frame.

---

## 2. Common identity and time semantics

Every durable tracker record needs:

- stable non-zero `record_identity` for logical dedupe/correlation;
- record kind/schema version;
- an observation/occurrence/result time value when trusted wall-clock time is
  available;
- explicit time-validity/quality state so zero/unknown time is never presented
  as a real timestamp;
- enough ordering identity to remain unambiguous across reboot/incarnation.

The exact persistent identity construction remains SF5C work. The exact
protected transport identity binding remains SF5B work.

Backend/app must preserve observation time separately from Gateway/Edge/backend
receipt time.

---

## 3. PERIODIC_OBSERVATION

One record represents one configured report period.

Required semantic fields:

### 3.1 Period

- `period_duration_seconds`;
- period end/observation time when valid;
- time-validity/quality.

The report period is the configured tracking/report interval in effect for that
record. A later config change does not reinterpret old records.

### 3.2 Location

Required:

- location validity;
- location source;
- latitude/longitude when valid;
- altitude when available/valid;
- location observation time/freshness relation to the report record;
- GNSS/location quality sufficient for the current product UI and validation
  path (currently HDOP + satellites where GNSS is the source).

Location is source-neutral. GNSS-specific quality must not make FIXED/PHONE or a
future reviewed source pretend to be GNSS.

A report record may exist with no fresh valid location. Missing location must be
represented explicitly rather than using 0,0.

### 3.3 Activity summary

The summary covers the same report period.

Required baseline semantics:

- `coverage_seconds`: amount of the report period backed by usable activity
  evidence;
- `unknown_seconds`: uncovered/unclassifiable duration;
- bounded movement/activity summary sufficient to answer whether the animal was
  inactive vs active over the interval;
- bounded transition/intensity evidence needed by the selected first product
  classifier.

Behavior labels such as grazing/walking/lying may be added to this schema only
when their classifier semantics and field evidence are reviewed. The durable
format must leave a versioned path for such validated classes, but SF5A does not
claim the current 5-second ActivityWindow can classify them.

Raw 10 Hz accelerometer samples and every internal 5-15 second feature window
are not durable product records.

Invariant:

```text
coverage_seconds + unknown_seconds <= period_duration_seconds
```

and any classified-duration breakdown introduced later must not claim more
covered time than `coverage_seconds`.

### 3.4 Battery

Required:

- battery voltage in a semantic physical unit suitable for the reference
  platform (mV candidate);
- battery state/quality with an explicit UNKNOWN option.

A user-facing percentage is not authoritative until the battery model/curve is
physically validated. The semantic contract must not fake precision.

Routine battery sampling occurs only on an already-required wake. The normal
value is carried by the periodic observation rather than generating its own
record.

### 3.5 Bounded health summary

Required:

- compact current product-health state/flags;
- no unbounded string/debug payload;
- no per-retry history.

Persistent product-affecting fault transitions are EVENTs. Repeated transient
issues may contribute to bounded counters/status but do not create one durable
record per retry.

### 3.6 Current tracker telemetry scope

Do not add speculative generic TLV telemetry merely because future sensors may
exist.

For the current animal-tracker reference product, PERIODIC_OBSERVATION carries
the current meaningful bounded fields above. A future real sensor service may
version the product-observation schema after its semantics, cadence, energy and
RF cost are known.

---

## 4. EVENT

EVENT represents one stable occurrence lifecycle, not a notification reminder.

Required semantics:

- stable non-zero `event_occurrence_identity`;
- event type;
- lifecycle state at minimum `ACTIVE` / `CLEARED`;
- severity/class suitable for product QoS;
- occurrence/transition time + validity;
- optional relation to the most relevant tracker record/location when such
  relation exists;
- bounded type-specific context only when it changes the meaning of the event.

Examples:

- confirmed geofence OUTSIDE active/cleared;
- LOST active/cleared;
- reviewed low/critical battery transition;
- persistent sensor/radio/storage/security fault transition;
- future reviewed tamper/safety event.

The same still-active condition is not appended every wake.

App/backend owns reminder cadence, escalation, mute/acknowledgement and repeated
notifications.

An EVENT must not require a valid location in order to exist. If location is
unknown, the event remains real and location association is explicitly absent.

---

## 5. COMMAND_RESULT

RESULT is the target application's authenticated outcome, not transport
delivery.

Required semantics:

- stable correlation to the logical COMMAND;
- result class/status;
- bounded reason/error code;
- result time + validity;
- enough resulting application state to make the result meaningful and
  idempotent for the command family.

For desired-state ConfigStore mutation, the separately reviewed CAS contract is
authoritative. RESULT therefore needs to support:

- `APPLIED`;
- `ALREADY_SATISFIED`;
- stale/precondition failure;
- invalid/policy rejection;
- busy/temporarily unavailable;
- state-uncertain/precondition-unavailable where required;
- resulting config state token when the CAS contract says it is valid.

The exact opcode/result-code numeric registry and plaintext byte layout remain
SF5B work.

A duplicate COMMAND must not repeat a side effect merely because the previous
RESULT was lost.

Reclaiming an old RESULT observation does not erase independent durable
replay/idempotency/security state.

---

## 6. Storage and transmission are separate representations

The semantic product record is not required to be byte-identical to its
protected LoRa frame.

```text
semantic product record
    -> durable ObservationStore representation
    -> TLP v2 protected transport representation
```

SF5C may choose a fixed-slot storage representation optimized for deterministic
power-cut recovery.

SF5B may choose a compact protected plaintext representation optimized for RF,
provided it preserves the same product semantics and security bindings.

Do not serialize internal C++ structs directly as either persistent or wire
format.

---

## 7. Size discipline

The design target remains a fixed ObservationStore slot near 96 bytes if the
actual fields fit without semantic loss.

This is not a byte cap.

If the reviewed semantics require 112 or 128 bytes, increase the slot rather
than:

- drop time validity;
- drop source/quality;
- fake battery precision;
- remove EVENT/RESULT identity;
- collapse health/error meaning;
- merge delivery/replay/idempotency concepts.

Exact packing belongs to SF5C after SF5B semantic/wire review.

---

## 8. Capacity/cadence invariant

Routine write rate is one PERIODIC_OBSERVATION per configured report period.

Examples:

```text
3 min  -> 480 routine records/day
15 min -> 96 routine records/day
1 h    -> 24 routine records/day
24 h   -> 1 routine record/day
```

EVENT and COMMAND_RESULT are additional asynchronous records and must remain
sparse relative to normal tracking under healthy operation.

No separate activity, battery or routine health record is written merely because
those components were observed internally during the period.

---

## 9. Reset / incomplete-period semantics

A reset/power loss may interrupt an in-progress activity aggregation period.

SF5A does not authorize frequent flash checkpoints merely to preserve partial
activity RAM.

After reboot, the next product observation must not claim complete activity
coverage for time that was not actually observed. Coverage/unknown semantics
must expose the gap.

If later field evidence shows that long 12/24-hour report periods require
durable mid-period activity checkpoints, that becomes a measured power/storage
tradeoff and a separate reviewed change.

---

## 10. Explicitly deferred to later slices

SF5A does not freeze:

- behavior classifier labels/thresholds;
- battery LOW/CRITICAL thresholds;
- exact record identity/incarnation bytes;
- packet/family/opcode/result numeric values;
- exact plaintext/wire sizes;
- AEAD header/tag changes;
- custody-ACK bytes;
- ObservationStore page/record bytes;
- physical flash addresses beyond the parent candidate direction;
- Gateway -> Edge transport;
- offline-read grant cryptography.

Those belong to SF5B+ and physical/product validation.
