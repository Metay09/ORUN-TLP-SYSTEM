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

Before **production secure-envelope runtime** consumes this candidate contract,
remaining gates are:

- independent ORUN-specific host vectors for the candidate KDF + CCM inputs;
- matching RAK4630/RAK4631 KAT for those exact bytes;
- focused Bluefruit/SoftDevice/CC310 coexistence hardware proof;
- separately reviewed A2D replay-persistence implementation with power-cut
  fault injection and wear analysis;
- authority/backend A2D reserve-ahead crash/restore tests when that component
  exists.

The immediate next code-bearing slice should be the focused **CryptoCell +
Bluefruit/SoftDevice coexistence proof**, and it should reuse the exact candidate
ORUN KDF/nonce/CCM bytes so that its host vectors and RAK KAT close the first
three gates together rather than creating another synthetic format. The smallest
required device-side replay-persistence slice follows. Full v2 wire bytes should
not be implemented until those gates are closed.
