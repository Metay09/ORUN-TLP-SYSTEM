# ORUN TLP v2 Product Secure Wire / Custody Security Contract

Status: **SF5B CANDIDATE FOR INDEPENDENT SECURITY/PROTOCOL REVIEW — EXACT
CANDIDATE BYTES ARE DEFINED HERE, BUT NO PRODUCTION RUNTIME, CRYPTO ACTIVATION,
FLASH FORMAT OR PHYSICAL PASS IS AUTHORIZED.**

Baseline: `main@cdb9db172d178acc877b9315453e7adbd7947985` (SF5A merged).

Parent contracts:

- `docs/architecture/ORUN_TLP_V2_PRODUCT_OBSERVATION_STORAGE_CUTOVER.md`
- `docs/architecture/ORUN_TLP_V2_TRACKER_PRODUCT_DATA_CONTRACT.md`
- `docs/architecture/ADR_M7P6_SECURITY_ARCHITECTURE.md`
- `docs/architecture/ORUN_TLP_V2_DELEGATED_COMMAND_SECURITY_DIRECTION.md`
- `docs/architecture/ORUN_CONFIG_STATE_TOKEN_CAS_DIRECTION.md`
- `docs/architecture/ORUN_GATEWAY_DURABLE_CUSTODY.md`

This slice deliberately does **not** implement codecs, activate RF paths, allocate
the 256 KiB flash region, enable Gateway custody, select the DFU model, or claim
that offline phone decryption is already solved.

---

## 1. Purpose

SF5A fixed the product meaning:

```text
PERIODIC_OBSERVATION
EVENT
COMMAND_RESULT
```

SF5B fixes the first reviewed candidate transport mapping needed before
ObservationStore bytes can be designed:

```text
PERIODIC_OBSERVATION -> PRODUCT_SECURE / DEVICE_D2A
EVENT                -> PRODUCT_SECURE / DEVICE_D2A
delegated config RESULT
                     -> existing DELEGATED_SECURE_APP / DELEGATED_D2GW
```

The existing SF2/SF3 `HISTORY_SECURE` bytes remain frozen. SF5 does not widen
or reinterpret them.

The product objective is compact authenticated/confidential device traffic that
an ordinary Gateway/relay can carry opaquely, while keeping logical product
identity independent from security counters and exact transport objects.

---

## 2. v2 type registry after this candidate

Version is always `0x02`.

```text
type 0x01  DELEGATED_SECURE_APP       frozen by M7P6H
type 0x02  V2_RELAY_FORWARD           candidate frozen by SF5B if review passes
type 0x03  HISTORY_SECURE              frozen SF2/SF3 legacy store-forward
type 0x04  PRODUCT_SECURE              new SF5 product observation/event path
type 0x05  GATEWAY_CUSTODY_ACK         new exact-object durable-custody ACK
```

No retired ID is reused.

`0x03 HISTORY_SECURE` remains decode/compatibility evidence until the explicit
SF5F cutover. New SF5 product records do not use its 29-byte plaintext or
73-byte fixed observation frame.

---

## 3. PRODUCT_SECURE envelope

### 3.1 Header

All multibyte integers are big-endian.

```text
off  size  field
0    1     version = 0x02
1    1     type = 0x04                    // PRODUCT_SECURE
2    1     security_context = 0x02        // DEVICE_D2A
3    1     app_family
4    1     path_flags
5    1     security_profile
6    1     ciphertext_len
7    1     reserved = 0

8    8     device_id
16   4     key_epoch
20   8     security_counter

28   N     ciphertext
28+N 8     authentication tag for profile 0x01
```

Current header size: **28 bytes**.

For current `security_profile = 0x01`, AAD is exactly bytes `0..27`.

The visible `device_id` is routing/key-lookup metadata, not authentication
evidence until AEAD succeeds and credential binding is checked.

### 3.2 Security profile

Initial registry:

```text
0x01 ROOT_AEAD_V1
```

Unknown profiles reject. There is no unauthenticated downgrade/fallback.

Profile `0x01` reuses the already reviewed M7P6 root-credential traffic
construction:

```text
PRK =
  HKDF-Extract(
    SHA-256,
    salt = credential_id[16],
    IKM = K_root[32])

traffic info =
  ASCII("ORUN-TLP-V2-AEAD")
  || direction_u8 = 0x01                  // D2A
  || key_epoch_be32

K_traffic = HKDF-Expand(..., 16 bytes)

nonce =
  key_epoch_be32
  || 0x01                                 // D2A direction
  || security_counter_be64

AEAD = AES-128-CCM
tag  = 8 bytes
```

The device uses the existing durable D2A counter allocator. A returned counter
is never put back into the pool.

The same exact protected frame may be retransmitted byte-for-byte. Changed AAD
or plaintext requires a fresh security counter.

### 3.3 Why security_profile exists

Profile `0x01` is a backend/trusted-authority root-AEAD profile. Its traffic
key must **not** be copied to a phone/Edge as an offline "read key", because a
holder of a symmetric AEAD key can also mint authenticated traffic under that
profile.

The explicit authenticated `security_profile` byte is the compatibility seam
for a later independently reviewed read-capable profile. That future profile may
change the protected-body/key composition while preserving the same product
plaintext schemas and routing header.

SF5B therefore does **not** claim offline local plaintext. It does ensure that
the first product wire does not require distributing tracker `K_root` or the
profile-0x01 D2A traffic key to a phone.

A later read-capable profile must use mature standard cryptography, must preserve
tracker-origin authenticity against a read-only holder, and requires its own
vectors/physical crypto proof before activation.

---

## 4. PRODUCT_SECURE application-family registry

Initial matrix:

```text
security_context 0x02 DEVICE_D2A
  app_family 0x01 PERIODIC_OBSERVATION
  app_family 0x02 EVENT
```

Other combinations reject.

`COMMAND_RESULT` is not forced into this root envelope merely to make one
universal packet. The first protected command path already has a dedicated
frozen delegated envelope and requires an offline gateway-readable RESULT.

---

## 5. Path flags

For `PRODUCT_SECURE`:

```text
bit0  RELAY_ALLOWED
bit1  CUSTODY_REQUESTED
bits2..7 = 0
```

Unknown bits reject.

These bits are authenticated AAD.

### RELAY_ALLOWED

A v2 relay may wrap the exact unchanged PRODUCT_SECURE object only when this bit
is set.

### CUSTODY_REQUESTED

This is a request to an authorized Gateway to attempt reviewed durable custody.

It does **not** mean the Gateway has accepted custody.

A Gateway may emit `GATEWAY_CUSTODY_ACK` only after durable commit/readback of
the exact inner protected object.

When the bit is clear:

- ordinary forwarding/backend delivery may still occur;
- the tracker retains responsibility;
- RF receipt/TX_DONE is not custody;
- the Gateway must not use receipt alone to make the tracker release the record.

This keeps routine live traffic from requiring an ACK on every report while
preserving store-first ownership.

A critical EVENT may be sent immediately as a live frame even if another custody
attempt is already outstanding. Durable custody can be acquired separately.

Changing `CUSTODY_REQUESTED` changes authenticated content and therefore
requires a fresh security counter/object.

---

## 6. Logical product record identity

Both initial PRODUCT_SECURE plaintext families contain:

```text
record_incarnation : u64, non-zero
record_sequence    : u32, non-zero
```

Logical product-record identity is:

```text
(DeviceIdentity, record_incarnation, record_sequence)
```

This identity survives retransmission, relay path, Gateway choice, D2A security
counter change and credential transport retry.

`record_sequence` never silently wraps. Exhaustion requires a new reviewed
incarnation/re-baseline.

SF5C owns the exact durable allocation/recovery mechanism. It must not use the
D2A security counter as the application record identity.

---

## 7. PERIODIC_OBSERVATION plaintext v1

Family:

```text
PRODUCT_SECURE / app_family 0x01
schema = 0x01
ciphertext_len = 64
total protected frame = 28 + 64 + 8 = 100 bytes
```

Exact plaintext:

```text
off size field
0   1    schema = 0x01
1   1    record_flags
2   1    location_source
3   1    battery_state

4   8    record_incarnation
12  4    record_sequence
16  4    period_end_epoch_seconds
20  4    period_duration_seconds

24  4    latitude_e7
28  4    longitude_e7
32  4    altitude_mm
36  2    hdop_x100
38  2    location_age_seconds
40  1    satellites
41  1    activity_quality
42  1    battery_quality
43  1    reserved = 0

44  4    activity_coverage_seconds
48  4    active_seconds
52  2    activity_transition_count
54  2    movement_mean_abs_delta_mg

56  2    battery_mv
58  2    health_flags
60  1    gnss_failure_count
61  1    sensor_failure_count
62  1    storage_failure_count
63  1    radio_failure_count
```

### 7.1 record_flags

```text
bit0 PERIOD_TIME_VALID
bit1 LOCATION_VALID
bit2 ALTITUDE_VALID
bit3 LOCATION_AGE_VALID
bit4 ACTIVITY_VALID
bit5 BATTERY_VALID
bit6 HEALTH_VALID
bit7 = 0
```

Unknown/reserved bits reject.

When `PERIOD_TIME_VALID=0`, `period_end_epoch_seconds=0`.

When `LOCATION_VALID=0`:

- coordinates are ignored and encoded as zero;
- `location_source=UNKNOWN`;
- `ALTITUDE_VALID=0` and `LOCATION_AGE_VALID=0`;
- altitude/age/GNSS-only quality fields are encoded as their non-authoritative
  values.

When `LOCATION_VALID=1`, `location_source` must be a non-UNKNOWN supported
source and latitude/longitude must pass range validation.

No consumer interprets `0,0` as the missing-location sentinel; validity is the
authority.

### 7.2 location_source

Wire semantic registry:

```text
0 UNKNOWN
1 GNSS
2 FIXED
3 PHONE
4 MANUAL
```

This registry does not imply all sources are implemented today.

For non-GNSS sources, `hdop_x100` and `satellites` are non-authoritative and
encoded as zero.

`location_age_seconds` uses:

```text
0x0000..0xFFFD exact age seconds
0xFFFE         saturated: age >= 65534 seconds
0xFFFF         unknown / not authoritative
```

The age field is used only when `LOCATION_AGE_VALID=1`. It means the elapsed
age of the referenced location at PERIODIC finalization. It may be derived from
trusted monotonic elapsed time and therefore does **not** require
`PERIOD_TIME_VALID`; it must never be fabricated from an untrusted wall clock.

### 7.3 Activity semantics

For a finalized PERIODIC record:

```text
0 <= active_seconds <= activity_coverage_seconds <= period_duration_seconds
unknown_seconds = period_duration_seconds - activity_coverage_seconds
inactive_seconds = activity_coverage_seconds - active_seconds
```

When `ACTIVITY_VALID=0`, activity classification fields are non-authoritative;
coverage may still expose how much sensor evidence existed.

`movement_mean_abs_delta_mg` is the bounded report-period aggregate of usable
short-window acceleration change evidence. It is not a grazing/walking/lying
classifier label.

`activity_quality` initial registry:

```text
0 UNKNOWN
1 PARTIAL
2 COMPLETE
```

For schema v1 the relationship is strict:

```text
coverage == 0                      -> quality = UNKNOWN, ACTIVITY_VALID=0
0 < coverage < period_duration    -> quality = PARTIAL, ACTIVITY_VALID=1
coverage == period_duration       -> quality = COMPLETE, ACTIVITY_VALID=1
```

When `ACTIVITY_VALID=0`, `active_seconds`,
`activity_transition_count` and `movement_mean_abs_delta_mg` are encoded
zero/non-authoritative.

`activity_transition_count=0xFFFF` means saturated at >=65535.
`movement_mean_abs_delta_mg=0xFFFF` means saturated at >=65535 mg.

A reset/power gap therefore cannot be reported as complete coverage.

### 7.4 Battery

`battery_mv` is measured voltage when `BATTERY_VALID=1`.

`battery_state`:

```text
0 UNKNOWN
1 NORMAL
2 LOW
3 CRITICAL
```

The LOW/CRITICAL thresholds are **not** frozen by SF5B; they remain
hardware/battery qualification policy.

`battery_quality`:

```text
0 UNKNOWN
1 MEASURED
2 MODEL_ESTIMATED
```

A percentage is not carried in v1 of this schema.

When `BATTERY_VALID=0`, `battery_mv=0`, `battery_state=UNKNOWN` and
`battery_quality=UNKNOWN`.

### 7.5 Health

`health_flags` is a bounded product-health bitmap. Initial bits:

```text
bit0  GNSS_DEGRADED
bit1  GNSS_FAULT
bit2  SENSOR_DEGRADED
bit3  SENSOR_FAULT
bit4  RADIO_DEGRADED
bit5  RADIO_FAULT
bit6  STORAGE_DEGRADED
bit7  STORAGE_FAULT
bit8  SECURITY_DEGRADED
bit9  SECURITY_FAULT
bit10 TIME_UNTRUSTED
bits11..15 = 0
```

The four failure-count bytes are **report-period saturating counters**:

```text
0..254 exact count during represented period
255    >=255
```

When `HEALTH_VALID=0`, health flags and all four counters are zero and
non-authoritative.

They are not lifetime counters and do not create one flash record per retry.

---

## 8. EVENT plaintext v1

Family:

```text
PRODUCT_SECURE / app_family 0x02
schema = 0x01
ciphertext_len = 48
total protected frame = 28 + 48 + 8 = 84 bytes
```

Exact plaintext:

```text
off size field
0   1    schema = 0x01
1   1    event_type
2   1    lifecycle_state
3   1    severity
4   1    event_flags
5   1    reason_code
6   1    location_source
7   1    context_kind

8   8    record_incarnation
16  4    record_sequence
20  4    transition_epoch_seconds
24  8    occurrence_id

32  4    latitude_e7
36  4    longitude_e7
40  2    location_age_seconds
42  2    context_flags
44  4    context_value
```

### 8.1 Event identity/lifecycle

`occurrence_id` is non-zero and stable for one occurrence.

```text
ACTIVE  -> same occurrence_id -> CLEARED
CLEARED -> later genuine reoccurrence -> new occurrence_id
```

The record identity and occurrence identity are different:

```text
record identity     = one immutable EVENT transition record
occurrence identity = the product condition lifecycle
```

Open-occurrence state remains durable control state as required by SF5A. This
wire does not make the historical ACTIVE record its only owner.

### 8.2 Initial event registry

`event_type`:

```text
0x01 GEOFENCE_OUTSIDE
0x02 LOST
0x03 BATTERY_STATE
0x04 SUBSYSTEM_FAULT
```

Additional types require a schema/registry review; do not allocate speculative
tamper/actuator events before a real product owner exists.

`lifecycle_state`:

```text
0x01 ACTIVE
0x02 CLEARED
```

`severity`:

```text
0x01 INFO
0x02 WARNING
0x03 CRITICAL
```

### 8.3 Event flags/location context

`event_flags`:

```text
bit0 TRANSITION_TIME_VALID
bit1 LOCATION_VALID
bit2 LOCATION_AGE_VALID
bit3 CONTEXT_VALID
bits4..7 = 0
```

EVENT does not require location. If no trustworthy location exists,
`LOCATION_VALID=0`.

The event carries latitude/longitude + age rather than assuming the backend has
already ingested the related PERIODIC record.

### 8.4 Context

Initial `context_kind`:

```text
0 NONE
1 BATTERY_MV
2 SUBSYSTEM_ID
```

For BATTERY_STATE, `BATTERY_MV` may carry the measured voltage that triggered
the confirmed transition.

For SUBSYSTEM_FAULT, `SUBSYSTEM_ID` identifies the bounded product subsystem.
Exact subsystem IDs are an application registry, not driver error-code leakage.

`context_flags` currently must be zero.

Schema-v1 relationship rules:

- if `TRANSITION_TIME_VALID=0`, `transition_epoch_seconds=0`;
- if `LOCATION_VALID=0`, location source/coordinates/age are encoded as
  UNKNOWN/zero and `LOCATION_AGE_VALID=0`;
- if `LOCATION_VALID=1`, source must be non-UNKNOWN and coordinates must be in
  range;
- if `CONTEXT_VALID=0`, `context_kind=NONE` and `context_value=0`;
- if `CONTEXT_VALID=1`, `context_kind` must be a supported non-NONE kind.

For `BATTERY_STATE`, initial `reason_code` values are:

```text
0x01 LOW
0x02 CRITICAL
```

A LOW occurrence may be CLEARED before a distinct CRITICAL occurrence is
activated; LOW and CRITICAL are not silently collapsed into one changing
occurrence identity.

For the other initial event types, schema-v1 `reason_code=0` unless a later
reviewed schema/registry explicitly assigns a meaning.

---

## 9. Immutable delayed-ingest semantics

PERIODIC and EVENT are immutable store-forward observation families.

After successful AEAD authentication:

- backend acceptance is not rejected merely because the D2A security counter is
  older than the ordinary bounded reordering window;
- backend dedupe is by
  `(DeviceIdentity, record_incarnation, record_sequence)`;
- same logical identity + equivalent authenticated semantic content is
  idempotent;
- same logical identity + different authenticated semantic content is an
  integrity conflict, never silent first-writer-wins;
- planned old-epoch decrypt-only retention remains bounded by the reviewed
  authoritative retirement ceiling;
- suspected/confirmed credential compromise still routes late old-epoch objects
  to quarantine/recovery policy rather than automatic canonical ingest.

This is the SF5 successor to the narrow SF4 HISTORY_SECURE immutable-observation
exception. It does not relax COMMAND/RESULT replay/freshness rules.

---

## 10. Exact protected-object lifetime and retry

Logical record identity and exact protected object identity remain different.

For one logical PERIODIC/EVENT record:

1. a best-effort live frame may be protected once with
   `CUSTODY_REQUESTED=0`;
2. a later durable-custody attempt may protect the same logical record with a
   fresh counter and `CUSTODY_REQUESTED=1`;
3. retransmissions of one active custody attempt are byte-identical;
4. a security counter is never reused to rebuild changed bytes.

Initial tracker bound:

> At most **4 exact custody-requested protected objects** may be outstanding at
> once for one tracker.

SF5C must durably retain/recover the exact bytes (or an independently audited
equivalent exact-object representation) for those bounded outstanding attempts.
It must not cache every historical record as a second full wire copy.

A power cut before the exact protected object becomes durably cache-authoritative
may force a fresh counter/object on recovery. That failure path must be
fault-injected and counted. The design must bound duplicate amplification; every
reset must not regenerate the complete backlog.

One logical record may have only one active custody-request object at a time.

Authenticated custody ACK may arrive out of record order. Tracker release state
therefore remains selective/bounded as required by SF5A.

---

## 11. V2_RELAY_FORWARD

SF5B adopts the already documented delegated-command wrapper candidate as the
shared one-hop v2 opaque relay wrapper.

Exact layout:

```text
off size field
0   1    version = 0x02
1   1    type = 0x02                    // V2_RELAY_FORWARD
2   1    hop_count = 1
3   1    inner_len
4   8    relay_device_id
12  2    ingress_rssi_dbm
14  1    ingress_snr_db
15  1    reserved = 0
16  N    exact unchanged inner frame
```

Relay metadata is untrusted path observation.

Nested v2 relay wrappers reject.

Initial inner allow-list:

```text
0x01 DELEGATED_SECURE_APP
0x04 PRODUCT_SECURE
0x05 GATEWAY_CUSTODY_ACK
```

Frozen legacy `0x03 HISTORY_SECURE` is not silently added to this wrapper.

For PRODUCT_SECURE, authenticated `RELAY_ALLOWED` must be set.

For GATEWAY_CUSTODY_ACK, ACK `RELAY_ALLOWED` must be set.

Relay may not modify the inner frame.

Gateway custody stores/binds the exact **inner protected object**, not the
16-byte relay metadata wrapper.

---

## 12. GATEWAY_CUSTODY_ACK

Custody ACK is a security authority separate from COMMAND and
BACKEND_DURABLE authority.

It has no confidentiality requirement. The candidate uses standard SHA-256,
HKDF-SHA256 and HMAC-SHA256; no ORUN-designed primitive is introduced.

### 12.1 Per-tracker/per-gateway ACK key

Start from the same tracker credential PRK:

```text
PRK =
  HKDF-Extract(
    SHA-256,
    salt = credential_id[16],
    IKM = K_root[32])
```

Derive:

```text
ack_info =
  ASCII("ORUN-TLP-V2-GW-CUSTODY-ACK-v1")
  || device_id_be64
  || key_epoch_be32
  || gateway_device_id_be64
  || gateway_policy_floor_be32
  || gateway_grant_generation_be32

K_custody_ack =
  HKDF-Expand(
    SHA-256,
    PRK,
    ack_info,
    32 bytes)
```

The backend/security authority may provision only `K_custody_ack` to the
specific enrolled Gateway. The Gateway does not receive `K_root`, the PRK, the
D2A traffic key, or command scope secrets merely because it owns custody ACK
authority.

No site/fleet/group custody key exists.

Exact KDF bytes require independent host vectors and matching RAK vectors before
runtime use.

### 12.2 Exact-object digest

For the exact inner protected custody object:

```text
object_digest = SHA-256(exact_frame_bytes[0..object_len-1])
object_ref    = first 8 bytes of object_digest
```

`object_ref` is only a bounded lookup hint. Security does **not** depend on its
64-bit collision resistance.

The ACK MAC binds the full 32-byte digest.

### 12.3 ACK wire

```text
off size field
0   1    version = 0x02
1   1    type = 0x05                    // GATEWAY_CUSTODY_ACK
2   1    flags
3   1    object_len

4   8    tracker_device_id
12  8    gateway_device_id
20  4    key_epoch
24  4    gateway_policy_floor
28  4    gateway_grant_generation
32  8    object_ref

40  16   hmac_tag
```

Total: **56 bytes**.

`flags`:

```text
bit0 RELAY_ALLOWED
bits1..7 = 0
```

MAC input:

```text
ASCII("ORUN-TLP-V2-CUSTODY-ACK-MAC-v1")
|| ack_bytes[0..39]
|| object_digest[32]
```

```text
full_mac = HMAC-SHA256(K_custody_ack, mac_input)
hmac_tag = first 16 bytes of full_mac
```

The 128-bit truncated HMAC is the ACK authenticator. The 8-byte object_ref is not
used as a substitute for HMAC verification.

### 12.4 Tracker ACK acceptance

Tracker accepts an ACK only if:

1. bounded length/version/type/reserved flags are valid;
2. `tracker_device_id` is local;
3. key epoch/policy floor/grant generation identify a currently acceptable
   custody authority;
4. gateway ID is an authorized custody-capable Gateway for that generation;
5. one bounded outstanding exact object matches `object_len + object_ref`;
6. tracker recomputes the **full** SHA-256 digest of that exact object;
7. HMAC verifies under the matching `K_custody_ack`;
8. the ACK maps unambiguously to one retained logical record;
9. release-state mutation is durably admitted under SF5C rules.

If zero candidates or more than one candidate remain after full verification,
release fails closed.

Replaying the same valid ACK is idempotent and consumes no ACK counter.

A compromised enrolled custody Gateway can still ACK and then discard data.
That is the already documented custody trust-anchor risk; cryptography cannot
prove honest flash behavior after enrollment.

---

## 13. Gateway pre-storage admission threat

PRODUCT_SECURE profile 0x01 remains opaque to the Gateway. A custody Gateway
cannot verify the tracker root AEAD before storage.

Therefore forged structurally valid frames can still create an availability /
flash-wear attack before backend authentication.

SF5B does **not** hide this limitation.

Before production custody runtime, SF5G must either:

- add an independently reviewed custody-admission authenticator that preserves
  multi-Gateway behavior and does not reveal `K_root`; or
- receive explicit independent security acceptance of bounded residual risk with
  enrolled-source filtering plus strict per-source/global RAM/flash admission,
  rate, wear and capacity limits.

If an admission authenticator is required, it must be a **separate outer
custody-admission/control object** around the already-frozen exact
PRODUCT_SECURE/RESULT custody object. It must not silently change the
PRODUCT_SECURE plaintext/header bytes after SF5B, and Gateway durable custody /
ACK identity continues to bind the exact protected inner object. This preserves
the product wire while leaving one explicit later admission-security seam.

Untrusted traffic may never evict already accepted custody.

No group admission secret is introduced in SF5B.

---

## 14. Delegated CONFIG COMMAND plaintext v1

The existing DELEGATED_SECURE_APP envelope remains exactly frozen by M7P6H.

SF5B freezes only the first application plaintext needed by the existing
ConfigStore/CAS product path.

Initial delegated scope registry:

```text
scope_id 0x01 CONFIG_STATE
```

Initial opcode registry:

```text
0x01 CONFIG_SET_DESIRED
0x02 CONFIG_STATE_READ
```

### 14.1 CONFIG_SET_DESIRED

Exact 32-byte COMMAND plaintext:

```text
off size field
0   1    schema = 0x01
1   1    opcode = 0x01
2   1    args_len = 8
3   1    flags = 0

4   8    command_id
12  12   expected_state_token

24  4    tracking_interval_seconds
28  4    battery_capacity_mah
```

`command_id` is non-zero and stable across cryptographic retries.

The complete 8-byte current ConfigStore semantic state is carried; this is not a
blind field patch.

### 14.2 CONFIG_STATE_READ

Exact 4-byte COMMAND plaintext:

```text
off size field
0   1    schema = 0x01
1   1    opcode = 0x02
2   1    args_len = 0
3   1    flags = 0
```

The authenticated DELEGATED_GW2D envelope `security_counter` is the exact
request-attempt correlation ID. This read has no side effect and does not require
a persistent command_id journal.

---

## 15. Delegated RESULT plaintext v1

The DELEGATED_D2GW envelope remains the frozen M7P6H 56-byte header with
32-byte maximum ciphertext.

### 15.1 Mutation RESULT

Exact 32-byte plaintext:

```text
off size field
0   1    schema = 0x01
1   1    result_code
2   1    result_flags
3   1    detail_code

4   8    command_id
12  8    request_counter
20  12   resulting_state_token
```

`request_counter` is the authenticated GW2D security counter of the exact
COMMAND attempt that produced/reconstructed this RESULT.

`result_flags`:

```text
bit0 TOKEN_VALID
bits1..7 = 0
```

When `TOKEN_VALID=0`, token bytes are all zero and are not cache-authoritative.

Initial result codes:

```text
0x01 APPLIED
0x02 ALREADY_SATISFIED
0x03 STALE_PRECONDITION
0x04 INVALID_ARGUMENT
0x05 POLICY_REJECTED
0x06 BUSY
0x07 STATE_UNCERTAIN
0x08 PRECONDITION_UNAVAILABLE
0x09 OUTCOME_UNKNOWN
```

Authentication/replay rejection does not emit one of these RESULTs.

`OUTCOME_UNKNOWN` preserves the ConfigStore v2 contract when a physical save
outcome cannot yet be proven. It is not "definitely failed".

For `APPLIED`, a valid resulting token is required.

For `ALREADY_SATISFIED`, a token is included only when token state is VALID.

For `STALE_PRECONDITION`, a current token may be returned when valid, but the
sender must not replace its config/token cache from token alone; it performs the
reviewed coherent state read when needed.

`detail_code` is zero for the first config family unless a later reviewed
result-code-specific registry defines a bounded value.

### 15.2 CONFIG_STATE_READ RESULT

Exact 32-byte plaintext:

```text
off size field
0   1    schema = 0x01
1   1    result_code
2   1    result_flags
3   1    reserved = 0

4   8    request_counter
12  12   state_token
24  4    tracking_interval_seconds
28  4    battery_capacity_mah
```

Allowed result codes for this read:

```text
0x0A STATE_SNAPSHOT
0x06 BUSY
0x07 STATE_UNCERTAIN
0x08 PRECONDITION_UNAVAILABLE
```

`STATE_SNAPSHOT` requires `TOKEN_VALID=1` and one coherent config/token
snapshot.

BUSY/UNCERTAIN/UNAVAILABLE responses carry no authoritative token/config values.

### 15.3 Durable RESULT retry rule

ObservationStore owns one logical RESULT outcome for the admitted logical
command, not one record per new crypto retry.

When the same logical CONFIG_SET_DESIRED is retried:

- the application effect is not repeated merely because RESULT was lost;
- retained logical outcome or current durable CAS state is used;
- the newly emitted RESULT binds the **new** request_counter;
- creating the response transport frame may consume a fresh D2GW security
  counter/salt as required by the delegated envelope;
- this does not append another durable logical RESULT merely because the
  transport counter changed.

---

## 16. RESULT custody

DELEGATED_D2GW RESULT is important and sparse.

An authorized custody Gateway may durably accept the exact protected RESULT
object and return GATEWAY_CUSTODY_ACK under the same exact-object contract.

Because the existing delegated header does not have a CUSTODY_REQUESTED bit,
DELEGATED_D2GW RESULT is custody-eligible by family policy.

Custody does not mean APPLIED; RESULT plaintext does.

---

## 17. Sender/replay acceptance boundaries

### PRODUCT_SECURE

PERIODIC/EVENT are immutable device observations.

- D2A nonce safety uses the existing device SecurityStore counter.
- backend accepts delayed authenticated immutable objects under §9 semantics.
- exact retransmission reuses bytes, not encryption.
- re-protection uses a fresh counter and retains logical record identity.

### DELEGATED COMMAND/RESULT

Existing M7P6 delegated replay/freshness/CAS rules remain authoritative.

SF5B does not weaken:

- gateway quota;
- policy floor/generation;
- single-outstanding GW2D command rule;
- replay durable HWM;
- opcode/scope binding;
- CAS ordering;
- OUTCOME_UNKNOWN semantics.

---

## 18. Calculated RF size / airtime

Reference calculation only:

```text
SF11
BW 125 kHz
CR 4/5
preamble 8
explicit header
CRC enabled
low-data-rate optimization enabled
```

Calculated raw airtime:

| frame | bytes | airtime |
| --- | ---: | ---: |
| v1 POSITION | 34 | ~987.136 ms |
| legacy HISTORY_SECURE observation | 73 | ~1724.416 ms |
| SF5 EVENT | 84 | ~1888.256 ms |
| delegated max COMMAND/RESULT | 96 | ~2134.016 ms |
| SF5 PERIODIC | 100 | ~2215.936 ms |
| relay-wrapped EVENT | 100 | ~2215.936 ms |
| relay-wrapped PERIODIC | 116 | ~2461.696 ms |
| GATEWAY_CUSTODY_ACK | 56 | ~1396.736 ms |
| relay-wrapped custody ACK | 72 | ~1724.416 ms |

These are calculations, not RF measurements and not regulatory compliance
claims.

A direct PERIODIC + custody ACK pair costs about **3.613 s** raw airtime before
retry/collision overhead.

### 18.1 Flat-domain raw-load warning

If every PERIODIC required immediate direct custody ACK, raw channel occupancy
from only PERIODIC+ACK would be approximately:

| effective cadence | 5 nodes | 10 nodes | 30 nodes | 50 nodes |
| --- | ---: | ---: | ---: | ---: |
| 3 min | 10.0% | 20.1% | 60.2% | 100.4% |
| 15 min | 2.0% | 4.0% | 12.0% | 20.1% |
| 30 min | 1.0% | 2.0% | 6.0% | 10.0% |
| 60 min | 0.5% | 1.0% | 3.0% | 5.0% |

This excludes relays, retries, EVENT, COMMAND/RESULT, collisions and other
traffic.

Therefore:

- 30-50 nodes at a 3-minute SF11 cadence cannot be treated as a flat single-RF-
  domain normal operating target;
- routine live PERIODIC must not require an ACK merely because it was sent;
- custody acquisition/backlog drain must be paced;
- adaptive data rate/cadence, gateway placement and RF-domain partitioning remain
  system-level capacity tools.

SF5B does not silently change the current RF profile. Final regional
duty-cycle/compliance analysis remains a later evidence gate.

---

## 19. Decoder / malformed-input rules

Before crypto:

- exact supported total length;
- version/type;
- fixed header length for current profile;
- reserved bytes/bits;
- supported context/family/profile;
- family exact ciphertext length;
- basic public identity/epoch/counter bounds.

After successful crypto:

- plaintext schema;
- semantic enums;
- coordinate ranges;
- flag/value relationships;
- activity arithmetic;
- non-zero record/occurrence identities.

Malformed/unauthenticated input:

- produces no application EVENT/RESULT;
- does not mutate replay/application durable state;
- does not trigger reflection/error RF response;
- does not gain critical priority merely by setting visible bytes.

Codec structs are not wire serialization.

---

## 20. Compatibility / cutover

Before SF5F:

```text
live POSITION/relay -> frozen TLP v1
legacy backlog      -> frozen SF3 HISTORY_SECURE when provisioned
```

After explicit validated SF5F cutover:

```text
new PERIODIC/EVENT  -> PRODUCT_SECURE
new delegated RESULT
                    -> DELEGATED_SECURE_APP
legacy HistoryStore/HISTORY_SECURE new-record path
                    -> retired
```

No permanent dual-write.

Existing v1 golden fixtures and SF2/SF3 HISTORY_SECURE vectors remain unchanged.

A real mixed fleet may keep legacy decoders at Gateway/backend; new trackers do
not need to emit both formats by default.

---

## 21. Implementation/audit gates

Before production runtime:

1. independent architecture/security audit of this exact byte/security contract;
2. host golden vectors for PRODUCT_SECURE header/plaintexts and malformed cases;
3. host KDF/HMAC/SHA-256 vectors for custody ACK;
4. RAK4630/RAK4631 KAT/coexistence proof for any newly exercised crypto primitive
   and the existing CC310/Bluefruit ownership boundary;
5. ObservationStore SF5C exact-object cache/release/open-event/result semantics
   with deterministic power-cut tests;
6. Gateway CustodyStore version/geometry review for the actual 100-byte maximum
   SF5 product object rather than the frozen 73-byte SF4B object;
7. RF/load simulation and physical on-air validation;
8. only then production secure-RF wiring.

No host PASS is reported as physical PASS.
