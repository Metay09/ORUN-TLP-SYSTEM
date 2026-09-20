# M7P6D — Secure-envelope pre-wire security contract

Status: **DRAFT FOR OWNER / INDEPENDENT SECURITY REVIEW. NO PRODUCTION RUNTIME OR WIRE CHANGE.**

Baseline: `main@dca843856cd99ce129758c64379b0e0356617834` (M7P6C merged via PR #27).

Branch: `docs/m7p6d-security-prewire-contract`.

## 1. Purpose

M7P6C proved the pinned RAK4630/RAK4631 CryptoCell path for RFC5869
HKDF-SHA256 and RFC3610 AES-128-CCM, including the bounded negative-input and
recovery probes recorded in `docs/milestones/M7P6C.md`.

M7P6D is the next design gate. It freezes only the security ownership rules
needed before secure-envelope wire bytes are designed:

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

M7P6D freezes the following KDF shape for the first secure-envelope generation:

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
- No fleet/group authority key is introduced.
- No telemetry/command/message sub-key tree is invented yet. Additional purpose
  separation requires an actual later use case and a new explicit label.

The exact ORUN-specific derivation above requires an independent host vector and
a RAK4630 KAT before production secure-envelope use. M7P6C proved the primitive,
not these exact ORUN bytes.

### Standards basis

This contract follows RFC 5869's extract-then-expand construction: the random
public `credential_id` is a non-secret salt and the exact `info` bytes provide
protocol/purpose/direction/epoch context. It also follows the AEAD nonce
uniqueness requirement: distinct encryption invocations under one fixed key must
not reuse a nonce. AES-CCM itself remains the NIST SP 800-38C mode already
exercised by M7P6C.

## 4. AES-CCM nonce contract

For the current AES-128-CCM direction, M7P6D freezes a **13-byte nonce**:

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
- a counter issued and then abandoned because encryption/send fails is burned,
  never returned to the pool;
- a retransmission of one already-created secure frame may retransmit the exact
  same bytes; it must not re-encrypt changed AAD/payload under the old
  counter/nonce;
- any changed authenticated content requires a fresh TX counter;
- rollover is forbidden. Counter exhaustion fails closed.

The later wire specification must carry or unambiguously supply
`key_epoch` and `tx_counter` so the receiver can reconstruct the nonce. It
must not derive a nonce from legacy TLP v1 sequence numbers, boot time, wall
clock, RSSI/SNR or relay metadata.

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
- replay state is scoped by credential lifetime, key epoch and direction;
- a failed AEAD result never exposes plaintext to the application;
- a replayed authenticated frame is not application-delivered again;
- RF/path observations may still record that a duplicate copy was heard, without
  treating it as a new trusted application event.

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
- the device accepts only a counter strictly greater than its durable A2D replay
  high-water mark;
- duplicate/older counters are rejected after authentication;
- the accepted high-water mark must be durably committed **before** a protected
  application action is dispatched;
- if durable replay-state update fails, the protected downlink fails closed.

This trades tolerance for arbitrary downlink reordering for simple,
power-cut-safe device behavior. The authority can retry with a fresh secure
frame/counter when appropriate.

A power cut after replay-state commit but before application execution can leave
an authenticated command accepted but not executed. That is **not** solved by
the security counter. A later command layer must use `command_id`, durable
result/idempotency semantics and explicit accepted/executed/failed states.

M7P6D does not implement the device A2D replay record. SecurityStore v1 has no
RX replay field; adding it requires a separately reviewed persistence-format
slice rather than silently consuming ConfigStore/History/BLE-bond storage.

## 6. CryptoCell / Bluefruit ownership contract

M7P7B production firmware already initializes Bluefruit. In the pinned Adafruit
framework, Bluefruit security initializes the global `nRFCrypto`/CC310
facility. M7P6C intentionally ran in an isolated image and therefore did not
prove shared production use.

M7P6D freezes these ownership rules:

1. **Global lifecycle is platform/composition-owned.**
   Production secure-envelope code must not call `nRFCrypto.end()`.
2. **No crypto from ISR/radio/BLE callbacks.**
   ORUN AEAD/KDF work is requested from callbacks only by bounded data/event
   handoff and executes from the cooperative/main execution owner.
3. **At most one ORUN CryptoCell operation is in flight.**
   Do not introduce parallel ORUN AES/HKDF jobs.
4. **Bluefruit remains an independent framework user of CC310.**
   An ORUN-only mutex does not prove serialization with Bluefruit's internal
   LESC/security operations.
5. **Production secure RF is blocked on a coexistence proof.**
   Before ORUN uses CC310 in its normal packet path, a focused RAK4630 test must
   exercise the selected ORUN AES-CCM/HKDF path with Bluefruit/SoftDevice active,
   including connection/pairing security activity where practical.
6. If that coexistence proof fails, do not patch around it with timing guesses.
   The next design must explicitly choose a safe serialization/admission policy
   or a different reviewed crypto backend.

M7P6C's test-only `nRFCrypto.begin()/end()` lifecycle is not a production
template.

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
that pinned backend and only for the proven authenticated-decrypt path. A
`CRYS_FATAL_ERROR` from initialization, KDF, encryption, argument validation or
another operation is not an authentication rejection.

Any non-success authenticated decrypt result produces no consumable plaintext.

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

Before M7P6D can be treated as the basis for code:

- independent security review of KDF input/domain separation;
- independent review of nonce uniqueness across credential/epoch/direction/reset;
- replay/power-cut review, especially commit-before-dispatch semantics;
- review of opaque-gateway compatibility;
- review of Bluefruit/CC310 shared-lifecycle assumptions against the pinned
  framework;
- verify that no statement accidentally upgrades a gateway into key authority;
- verify that no TLP v1 or current persistence behavior is reinterpreted.

After approval, the next code-bearing slice should be the **CryptoCell +
Bluefruit/SoftDevice coexistence proof**, followed by the smallest required
device-side replay-persistence slice. Full v2 wire bytes should not be
implemented until those gates are closed.
