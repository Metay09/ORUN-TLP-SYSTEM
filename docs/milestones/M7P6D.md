# M7P6D — Secure-envelope pre-wire security contract

Status: **INDEPENDENTLY REVIEWED CANDIDATE CONTRACT; REVIEW CORRECTIONS APPLIED. NO PRODUCTION RUNTIME OR WIRE CHANGE.**

Baseline: `main@dca843856cd99ce129758c64379b0e0356617834` (M7P6C merged via PR #27).

Branch: `docs/m7p6d-security-prewire-contract`.

## 1. Purpose

M7P6C proved the pinned RAK4630/RAK4631 CryptoCell path for RFC5869
HKDF-SHA256 and RFC3610 AES-128-CCM, including the bounded negative-input and
recovery probes recorded in `docs/milestones/M7P6C.md`.

M7P6D is the next design gate. It records a **candidate contract** for the
security ownership rules needed before secure-envelope wire bytes are designed.
These exact KDF/nonce choices are not implementation-frozen until their ORUN-specific
host vectors, RAK KAT and Bluefruit/SoftDevice coexistence gate pass:

1. traffic-key direction separation;
2. nonce construction and TX-counter ownership;
3. replay-state semantics and ownership;
4. CryptoCell/Bluefruit lifecycle and concurrency boundary.

This slice deliberately does **not** allocate a v2 packet/application ID, define
the complete v2 header, choose the practical RF MTU, implement secure RF,
implement commands/ACKs, modify SecurityStore persistence, or change TLP v1.

## 2. Terminology

The cryptographic peer of a device is called the **trusted authority** in this
document.

For the first network design, the trusted authority is normally a backend or
another explicitly trusted endpoint that is provisioned for that device. A
gateway or relay is **not** a trusted authority merely because it transports a
frame.

Directions are therefore defined by trust endpoint, not by legacy Role:

- `D2A = 0x01`: device -> trusted authority;
- `A2D = 0x02`: trusted authority -> device.

This avoids binding security semantics to TRACKER/RELAY/BASE role names and
preserves the ADR rule that normal relays/gateways are opaque
custody/forwarding infrastructure.

## 3. Traffic-key derivation contract

M7P6D records the following candidate KDF shape for the first secure-envelope generation:

```text
IKM  = K_root                       // 32 bytes
salt = credential_id                // 16 bytes, public random credential lifetime ID

info =
    ASCII("ORUN-TLP-V2-AEAD")       // exactly 16 bytes, no NUL terminator
    || direction_u8                 // D2A=0x01, A2D=0x02
    || key_epoch_be32

PRK        = HKDF-Extract(SHA-256, salt=credential_id, IKM=K_root)
K_traffic  = HKDF-Expand(SHA-256, PRK, info, 16 bytes)
```

Consequences:

- D2A and A2D never use the same traffic key.
- A key epoch change changes the traffic key.
- A new random `credential_id` creates a separate derivation context even if a
  provisioning defect were ever to reuse other metadata.
- DeviceIdentity is deliberately **not** embedded in the KDF label. The public
  identity still belongs in the later authenticated envelope/AAD, but keeping it
  out of this KDF avoids prematurely freezing the future v2 on-air identity
  namespace to the current legacy uint64 representation.
- A receiver must resolve the security credential from an already trusted
  credential/device association, not trust an unauthenticated on-air identity
  claim. Any future on-air origin/destination identity that affects routing,
  dispatch or authorization must be authenticated as AAD and checked against
  the credential binding after AEAD success.
- No fleet/group authority key is introduced.
- No telemetry/command/message sub-key tree is invented yet. Additional purpose
  separation requires an actual later use case and a new explicit label.
- The KDF label is a compatibility identifier. Changing AEAD algorithm, traffic
  key length, authentication-tag length, nonce construction or another
  cryptographically material usage contract requires a new registered label;
  a later implementation must keep one explicit label registry rather than
  invent labels at call sites.

The exact ORUN-specific derivation above requires an independent host vector and
a RAK4630 KAT before production secure-envelope use. The same pre-wire vector
set must also cover the chosen ORUN CCM nonce/AAD construction and representative
payload lengths, including a zero-length protected payload if that form is
allowed. M7P6C proved the primitives, not these exact ORUN bytes.

### Standards basis

This contract follows RFC 5869's extract-then-expand construction: the random
public `credential_id` is a non-secret salt and the exact `info` bytes provide
protocol/purpose/direction/epoch context. It also follows the AEAD nonce
uniqueness requirement: distinct encryption invocations under one fixed key must
not reuse a nonce. AES-CCM itself remains the NIST SP 800-38C mode already
exercised by M7P6C.

## 4. AES-CCM nonce contract

For the current AES-128-CCM direction, M7P6D records the following candidate **13-byte nonce**:

```text
nonce[0..3]   = key_epoch_be32
nonce[4]      = direction_u8
nonce[5..12]  = tx_counter_be64
```

Rationale:

- the pinned CC310 path physically passed a 13-byte CCM nonce in M7P6C;
- 13 bytes is supported by the pinned implementation;
- the current radio receive ceiling is only 255 bytes, so the CCM length-domain
  associated with a 13-byte nonce is more than sufficient for ORUN frames;
- the unique durable 64-bit counter remains the uniqueness workhorse;
- epoch and direction are explicit domain separation in both KDF and nonce.

Invariant:

> The same nonce must never be used twice with the same traffic key.

Counter rules:

- a TX counter belongs to the **actual cryptographic sender and direction**;
- current M7P6B `SecurityStore::reserveNextTxCounter()` is the D2A device
  counter source;
- A2D needs a separate durable authority-side per-device counter; a gateway must
  not invent, rewrite or reserve it;
- for each `(credential_id, key_epoch, A2D)` security context there is exactly
  **one active cryptographic sender/authority owner**. A standby backend or
  site-local endpoint that possesses the same root may not independently issue
  A2D counters;
- the authority-side A2D counter must use reserve-ahead durability before any
  protected frame is encrypted: persist an absolute future bound, wait for
  durable completion, verify it by readback, and only then issue counters below
  that bound. Recovery skips unused reserved counters and must never resume
  below the highest durable bound;
- backend/database restore, failover or other rollback must not roll the A2D
  counter backward. If the highest safe bound is uncertain, protected A2D
  transmission fails closed until a new credential lifetime/root is established;
- a security counter is allocated at final encryption/TX admission, not when a
  logical application/history item merely enters a queue;
- a counter issued and then abandoned because encryption/send fails is burned,
  never returned to the pool;
- a retransmission of one already-created secure frame may retransmit the exact
  same bytes; it must not re-encrypt changed AAD/payload under the old
  counter/nonce;
- any changed authenticated content requires a fresh TX counter. A logical
  History/application record that is encrypted again after its prior secure
  frame is discarded therefore receives a fresh counter;
- rollover is forbidden. Counter exhaustion fails closed.

The later wire specification must carry or unambiguously supply
`key_epoch` and `tx_counter` so the receiver can reconstruct the nonce. It
must not derive a nonce from legacy TLP v1 sequence numbers, boot time, wall
clock, RSSI/SNR or relay metadata. The first wire proposal should prefer the
full 64-bit counter. Any truncation/reconstruction scheme needs a separate
review, may derive at most one nonce candidate per received frame from already
authenticated replay state, and must not let unauthenticated traffic fan out
into multiple AEAD attempts.

## 5. Replay contract

Replay protection is an authenticated-receive concern, separate from application
command idempotency.

### 5.1 Common receive ordering

A future protected receiver must follow this semantic order:

```text
bounded structural parse
-> optional non-mutating stale/obviously-invalid prefilter
-> derive/select traffic key
-> AEAD authentication/decrypt
-> replay admission
-> durable replay-state update when required
-> application dispatch
```

Rules:

- unauthenticated input never advances replay state;
- replay state is scoped by credential lifetime, current accepted key epoch and
  direction, but changing epoch must never make a retired epoch acceptable again;
- a failed AEAD result never exposes plaintext to the application;
- a replayed authenticated frame is not application-delivered again;
- RF/path observations may still record that a duplicate copy was heard, without
  treating it as a new trusted application event;
- the receiver accepts only its current key epoch unless a later explicitly
  designed rotation/grace protocol says otherwise. Retired epochs remain retired
  and their replay state is never reset in a way that makes old authenticated
  frames acceptable again;
- incrementing `key_epoch` while retaining the same `K_root` provides traffic
  key/nonce domain rotation, **not compromise recovery**. Compromise recovery
  requires a new secret credential lifetime.

### 5.2 D2A receiver: trusted authority/backend

Multiple gateways/relay paths can deliver the same device frame and legitimate
frames can arrive out of order.

Therefore the backend/trusted authority may use a **bounded sliding replay
window** keyed by device credential lifetime + key epoch. The exact bitmap width
is an implementation parameter to be selected from simulation/observed
reordering; it is not a wire-format field and is not frozen here.

Within that window:

- first authenticated occurrence is application-deliverable;
- later copies of the same counter are duplicates;
- too-old counters outside the window are rejected;
- replay state must survive backend restart.

### 5.3 A2D receiver: device

The first device-side downlink contract is intentionally simpler:

- one trusted authority serializes distinct protected downlinks per device;
- while one distinct A2D frame is outstanding, the authority does not advance to
  a later distinct frame for that device. Byte-identical transport
  retransmissions of the outstanding frame are allowed. After an attempt is
  abandoned/expired, a new logical retry uses a fresh security counter; a later
  command layer reuses the same `command_id` so application idempotency is
  independent of transport replay;
- the device accepts only a counter strictly greater than its durable A2D replay
  high-water mark;
- duplicate/older counters are rejected after authentication;
- the accepted high-water mark must be durably committed **before** a protected
  application action is dispatched. The persistence slice must use explicit
  commit-last/readback-verified power-cut semantics; ambiguous recovery must
  never lower the HWM and instead disables protected A2D reception fail-closed;
- if durable replay-state update fails, the protected downlink fails closed.

This trades tolerance for arbitrary downlink reordering for simple,
power-cut-safe device behavior. It also means replay protection alone provides
**no freshness guarantee**: an authenticated frame that was delayed and never
previously accepted can still be valid later. Trusted ACK/contact and command
families therefore need their own expiry/freshness or challenge semantics in
their later application milestone.

A power cut after replay-state commit but before application execution can leave
an authenticated command accepted but not executed. That is **not** solved by
the security counter. A later command layer must use `command_id`, durable
result/idempotency semantics and explicit accepted/executed/failed states.

M7P6D does not implement the device A2D replay record. SecurityStore v1 has no
RX replay field; adding it requires a separately reviewed persistence-format
slice rather than silently consuming ConfigStore/History/BLE-bond storage. That
slice must include M7P6B-style power-cut fault injection and a flash-wear/admission
budget for authenticated A2D commits. Unauthenticated traffic must never cause
flash wear. Service procedures must also treat restoration of an older raw
security-partition image as security rollback requiring re-provisioning rather
than silently resuming protected traffic.

## 6. CryptoCell / Bluefruit ownership contract

M7P7B production firmware already initializes Bluefruit. In the pinned Adafruit
framework, Bluefruit security initializes the global `nRFCrypto`/CC310
facility. M7P6C intentionally ran in an isolated image and therefore did not
prove shared production use.

M7P6D records these candidate ownership rules:

1. **Global lifecycle is platform/composition-owned.**
   Production secure-envelope code must not call `nRFCrypto.end()`. The pinned
   Adafruit `nRFCrypto.begin()` sets its internal begun flag before all
   initialization succeeds, so secure-feature readiness must not be inferred
   solely from a later `begin()==true`. Before secure RF is enabled, the
   composition root must perform one controlled initialization/readiness check
   and a small known-answer canary; failure keeps protected features disabled.
2. **No crypto from ISR/radio/BLE callbacks.**
   ORUN AEAD/KDF work is requested from callbacks only by bounded data/event
   handoff and executes from the cooperative/main execution owner.
3. **At most one ORUN CryptoCell operation is in flight.**
   Do not introduce parallel ORUN AES/HKDF jobs.
4. **Bluefruit remains an independent framework user of CC310.**
   An ORUN-only mutex does not prove serialization with Bluefruit's internal
   LESC/security operations. Adafruit's random provider also has shared global
   state; future ORUN RNG/provisioning use must be included in the same ownership
   review rather than assumed independent.
5. **Production secure RF is blocked on a coexistence proof.**
   Before ORUN uses CC310 in its normal packet path, a focused RAK4630 test must
   exercise the selected ORUN AES-CCM/HKDF path with Bluefruit/SoftDevice active,
   including connection/pairing security activity where practical.
6. If that coexistence proof fails, do not patch around it with timing guesses.
   The next design must explicitly choose a safe serialization/admission policy
   or a different reviewed crypto backend.

M7P6C's test-only `nRFCrypto.begin()/end()` lifecycle is not a production
template. The production backend must also satisfy the pinned CC310 DMA/input
requirements: validate lengths/pointers/overlap before the hardware call and
copy flash-resident nonce/AAD/KDF-info constants into suitable RAM buffers when
the API requires DMA-capable memory.

## 7. Error classification contract

The M7P6C compatibility observation remains narrow:

- `CRYS_OK` means cryptographic operation success;
- dedicated CCM MAC-invalid means authentication rejection;
- the exact pinned CC310 binary also physically returned
  `CRYS_FATAL_ERROR (0x00F50000)` for wrong-tag Finish/decrypt.

Future production code must not spread raw CC310 result codes into application
logic. It needs a narrow crypto boundary with at least:

```text
Success
AuthRejected
InvalidInput
EngineFailure
```

The M7P6C `CRYS_FATAL_ERROR` compatibility behavior may be handled only inside
that exact pinned backend and only for the proven Finish/decrypt path **after**
the wrapper has validated lengths, pointers, buffer overlap and other local
preconditions. A `CRYS_FATAL_ERROR` from initialization, KDF, encryption,
argument validation or another operation is an `EngineFailure`, not an
authentication rejection. A version-pinned startup/CI canary must confirm the
known bad-tag result class and immediate valid-decrypt recovery before protected
traffic is enabled.

Any non-success authenticated decrypt result produces no consumable plaintext;
the production wrapper clears its output buffer before returning failure.

## 8. What remains deliberately unfrozen

M7P6D does **not** decide:

- complete v2 header byte layout or packet/application IDs;
- practical RF MTU;
- origin/destination on-air encoding;
- credential/key identifier compression;
- AAD byte layout;
- final relay-visible header subset;
- ACK/contact semantics;
- command schema;
- command expiry/time policy;
- provisioning ceremony or backend key escrow;
- key-rotation/grace protocol;
- device-side RX replay persistent record format;
- secure-envelope production API/class layout;
- BLE commissioning authorization;
- DFU firmware-authenticity policy.

Those are separate decisions because they affect wire compatibility, persistence,
airtime, authorization or update trust.

## 9. Compatibility and system impact

This milestone is documentation only.

- TLP v1 TEST/POSITION/RELAY_FORWARD bytes: unchanged.
- Existing golden/compatibility fixtures: unchanged.
- RF frequency/SF/BW/CR/TX power: unchanged.
- Relay forwarding/dedupe: unchanged.
- Current TRACKER RX/power policy: unchanged.
- SecurityStore flash layout: unchanged.
- BLE runtime/admission: unchanged.
- GNSS/activity/geofence/history: unchanged.
- RAM/flash/runtime power: unchanged.

No physical behavior is claimed by this documentation slice.

## 10. Validation / review gate

Independent review found no BLOCKER and identified one HIGH plus two MEDIUM
pre-wire documentation gaps. This revision closes them by defining
authority-side reserve-ahead/rollback safety and single-sender ownership,
downgrading exact KDF/nonce wording to a candidate contract, and defining
current/retired epoch acceptance. It also incorporates the review's bounded
clarifications for identity/AAD binding, replay persistence, freshness,
serialization, counter allocation timing, counter truncation, KDF label
versioning, ORUN-specific vectors and CryptoCell readiness/error handling.

No build or physical test is claimed for M7P6D because this slice changes only
documentation.

The original gate list for production secure-envelope runtime was:

- independent ORUN-specific host vectors for the candidate KDF + CCM inputs;
- matching RAK4630/RAK4631 KAT for those exact bytes;
- focused Bluefruit/SoftDevice/CC310 coexistence hardware proof;
- separately reviewed A2D replay-persistence implementation with power-cut
  fault injection and wear analysis;
- authority/backend A2D reserve-ahead crash/restore tests when that component
  exists.

**2026-09-23 gate update:** M7P6E and its corrected fresh-pairing follow-up close
the first three gates for the exact pinned candidate path. The candidate D2A/A2D
KDF keys, 13-byte nonce and probe CCM result were independently regenerated on
the host, matched on RAK hardware, and the scoped Bluefruit/SoftDevice/CC310
fresh-pairing coexistence run passed. This does not freeze the final v2 header or
AAD and is not a claim of arbitrary CC310 thread-safety.

The immediate next device-side security foundation is therefore the smallest
reviewed **A2D replay high-water-mark persistence** slice. It must commit the
authenticated receive HWM durably before protected application dispatch, use
power-cut-safe fail-closed recovery, budget flash wear/admission, and ensure
unauthenticated traffic cannot cause flash mutation. It must not allocate final
v2 wire bytes merely to test persistence.

After that persistence gate is closed, the secure-envelope wire milestone may
freeze the complete v2 header/AAD/MTU and production codec/crypto boundary using
the already-proven KDF/nonce candidate. Authority/backend A2D reserve-ahead
crash/restore testing becomes mandatory when that cryptographic sender component
exists and before protected A2D traffic is called production-ready.

Provisioning/commissioning remains a separate prerequisite for using real
production credentials; its authority-key custody and ownership ceremony must be
reviewed before exposing a production credential-write transport.

## 11. Next device-side persistence slice candidate — A2D replay reserve/HWM

This section records the smallest implementation contract implied by §5.3 after
the M7P6E host/RAK/coexistence gates closed. It is **not** a v2 wire allocation
and does not authorize production protected downlink yet.

### 11.1 Ownership and non-goals

The replay state belongs to `SecurityStore`, not ConfigStore, HistoryStore,
BLE bond storage, Role or the future application command layer.

The persistence-only slice must not add:

- a TLP v2 header or packet/application ID;
- AES-CCM into the production RF path;
- provisioning/commissioning;
- a command/config/message operation;
- application dispatch from unauthenticated input;
- a new flash partition.

The existing two-page SecurityStore partition remains the exclusive owner:

```text
0x0E7000..0x0E8000  security page A
0x0E8000..0x0E9000  security page B
```

### 11.2 Why SecurityStore schema v1 cannot be extended in place

The v1 page is exactly full:

```text
32 B page header
68 B credential
111 * 36 B TX_RESERVE
----------------------
4096 B
```

There is no erased slot that can safely become replay state without changing
the on-flash schema. A2D replay persistence therefore requires an explicit
SecurityStore **format v2**. It must not reinterpret a v1 TX_RESERVE byte or
consume Config/History/Bond pages.

### 11.3 Candidate v2 state-record layout

Keep the existing 32-byte page header shape and 68-byte credential shape, but
bump the page format version to 2 and replace the TX-only tail with one bounded
typed append log.

Candidate 40-byte `SECURITY_STATE` record:

```text
offset  size  field
0       16    credential_id
16      4     key_epoch
20      1     kind
21      3     reserved = 0
24      8     value
32      4     CRC32
36      4     commit word = 0
```

The only authorized kinds in this slice are:

```text
1 = TX_RESERVE_EXCLUSIVE_BOUND
2 = A2D_REPLAY_EXCLUSIVE_BOUND
```

No generic plugin/state registry is implied.

With the existing header/credential sizes:

```text
floor((4096 - 32 - 68) / 40) = 99 state slots
36 bytes remain reserved/erased at the page tail
```

A shared typed log avoids an arbitrary fixed TX/RX partition: whichever security
state actually advances consumes the next slot. Compaction carries forward at
most the latest TX bound and latest A2D replay bound before page activation.

Recovery rules remain fail-closed:

- records are append-only; an erased gap followed by non-erased state is FAULT;
- CRC/commit/reserved-byte failure on authoritative committed state is FAULT;
- record credential_id and key_epoch must match the authoritative credential;
- unknown record kind in format v2 is FAULT; adding a new security-state semantic
  requires an explicit schema/version review;
- each bound is monotonic for its kind and may never roll backward.

### 11.4 A2D replay reserve semantics

Use an **exclusive durable replay bound**, analogous in shape but not meaning to
the TX reservation bound.

Candidate block size for the first implementation review:

```text
A2D replay reservation block = 8 counters
```

If the durable A2D bound is `B`, a reboot conservatively treats all counters
`< B` as already consumed for replay purposes. During the current boot,
SecurityStore may retain the exact last accepted counter in RAM and accept newer
authenticated counters still below `B` without another flash mutation.

For an already-authenticated A2D counter `c`:

1. if `c <= runtime_hwm`, reject as duplicate/old with **no flash write**;
2. if `runtime_hwm < c < durable_bound`, advance only the RAM HWM and allow
   the persistence owner to report replay admission complete;
3. if `c >= durable_bound`, compute the smallest block-aligned exclusive bound
   greater than `c`, append/commit/read-verify that bound, then advance the
   RAM HWM and report replay admission complete;
4. application dispatch is forbidden until step 3 has completed successfully
   when a durable advance was required;
5. any persistence ambiguity/failure rejects the protected A2D operation
   fail-closed.

On reboot, unused counters below the durable bound are deliberately burned. With
the candidate block size 8, at most seven not-yet-accepted counter values are
lost per replay-reservation boundary/reboot. This is an availability tradeoff,
not a security rollback.

Counter/bound overflow fails closed. The persistence contract does not invent a
counter-wrap or epoch-grace protocol.

### 11.5 Compaction snapshot ordering

A format-v2 compaction must preserve the M7P6B activation-last invariant:

1. erase inactive destination page;
2. write/verify page-header body with activation erased;
3. write/commit latest TX reserve state when present;
4. write/commit latest A2D replay reserve state when present;
5. write/commit credential;
6. program/read-verify page activation **last**;
7. only then switch RAM authority;
8. old-page cleanup remains maintenance work.

A power cut before activation leaves the previous page authoritative. A power
cut after activation is safe only because both security bounds and the
credential snapshot were already durable.

The implementation should retain one deterministic append-slot headroom before
starting compaction, as M7P6B already does.

### 11.6 v1 -> v2 migration contract

Do not erase or reset a valid v1 credential merely because the schema changes.

A new firmware that recovers an authoritative, device-bound v1 page may migrate
it only by the same A/B transaction discipline:

1. recover the v1 credential and highest durable TX bound without changing them;
2. build format v2 on the inactive page with generation + 1;
3. carry the TX bound into a v2 TX state record when non-zero;
4. create **no** A2D replay record yet (no protected A2D frame has been accepted);
5. write the credential;
6. activate the v2 page last;
7. erase the old v1 page only after v2 activation.

Cut before v2 activation -> v1 remains authoritative.
Cut after v2 activation -> v2 is authoritative.

Existing older firmware already treats a recognized newer security format as
UNSUPPORTED globally, so downgrade after a committed v2 page fails closed
instead of falling back to stale v1 security state.

FOREIGN, UNSUPPORTED or FAULT state must never auto-migrate.

### 11.7 Wear/capacity budget

A v2 page has 99 shared state slots. In steady state, carrying both current
bounds plus one deterministic headroom leaves about 96 advancing state records
before compaction.

The candidate A2D block size 8 is intentionally smaller than the TX block 256:
reboot should not strand hundreds of authority counters merely to save flash.

Illustrative worst-case arithmetic, **not a hardware-life guarantee**:

- 3-minute development cadence = 480 report opportunities/day;
- one protected D2A TX per report consumes about 1.875 TX-reserve records/day;
- an authenticated A2D downlink on every report consumes at most 60
  replay-reserve records/day at block size 8;
- combined worst-case ≈61.9 advancing state records/day;
- ≈96 / 61.9 = 1.55 days between compactions;
- ≈235 compactions/page/year, or ≈2,350 erase cycles/page over ten years,
  before pathological reset/additional-traffic effects.

At a 15-minute report cadence the same deliberately pessimistic
one-downlink-per-report model is far lower. Actual field traffic is expected to
be lower still because ordinary telemetry does not require an A2D response per
packet.

This arithmetic is sufficient to justify reserve-ahead over per-message flash
writes, but the exact block size remains a **candidate** until independent
security/storage review. Physical endurance claims still require the actual
silicon specification and representative field measurements.

### 11.8 API/dispatch boundary for the later implementation

The persistence slice should expose only a bounded internal replay-admission
operation/result. It must have no production RF caller yet.

The later secure receive path must preserve this ordering:

```text
bounded parse
-> credential/key selection
-> AEAD authenticate/decrypt
-> SecurityStore A2D replay admission
-> durable replay advance if required
-> application dispatch
```

Therefore unauthenticated traffic cannot cause SecurityStore flash mutation.
A later command layer still needs independent `command_id`/result/idempotency
state; replay persistence does not solve power loss between replay commit and
command execution.

### 11.9 Required validation for the persistence slice

Before this foundation can be called closed:

- exact v1 and v2 format golden/malformed tests;
- v1 -> v2 migration tests at every activation cut point;
- TX bound preservation across migration;
- A2D duplicate/old/new admission tests;
- no-flash path for authenticated counters already inside the durable reserve;
- reserve-bound crossing and large counter jump;
- counter/bound overflow fail-closed;
- torn body, torn commit, failed readback and append-gap recovery;
- compaction with TX-only, A2D-only and both bounds;
- reset property test proving no counter accepted after reboot is below the
  conservative durable replay bound;
- FOREIGN/UNSUPPORTED/FAULT never migrate;
- FlashMutationGate priority/timeout regression;
- full host warnings-as-errors + ASan/UBSan;
- normal RAK4630 production build and size comparison.

A real hardware persistence/reboot sentinel is desirable after software review,
but this design section makes no new physical claim.

