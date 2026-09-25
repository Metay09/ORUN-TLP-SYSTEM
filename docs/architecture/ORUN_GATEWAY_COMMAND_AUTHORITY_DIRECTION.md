# ORUN Gateway Command Authority and Offline Store-Forward Direction

Status: **OWNER-APPROVED PRODUCT / ARCHITECTURE DIRECTION — NO WIRE OR CRYPTO FORMAT FREEZE.**

Owner decision date: 2026-09-25.

This record captures the product/security direction agreed after
\`docs/audits/GATEWAY_COMMAND_AUTHORITY_EXPLORATION.md\` and the subsequent
owner review. It exists so the design is not recoverable only from chat history.

This document **does not** authorize implementation by itself. It does not freeze:

- TLP v2 bytes;
- secure-envelope byte layout;
- exact AEAD/KDF labels or nonce layout;
- exact gateway grant/certificate format;
- exact user-offline authorization proof;
- SecurityStore schema/version;
- bounded gateway replay slot count;
- revoke/expiry encoding;
- command-id persistence encoding.

TLP v1 compatibility and the existing M7P6 security rules remain regression
requirements until an explicit later milestone changes them.

## 1. Product behavior that is approved

The system must preserve this user-visible topology:

~~~text
                  BACKEND
                  /     \
                 /       \
        kullanıcı          gateway
        yetkileri          enrollment
            │                  │
            ▼                  ▼
         UYGULAMA ───────► HERHANGİ BİR
                              GATEWAY
                                │
                        güvenli tracker
                            paketi üret
                                │
                              RELAY
                        (gerekirse bekle)
                                │
                                ▼
                             TRACKER
~~~

The concepts are independent:

\`User Identity / User Authorization != Gateway Enrollment != Gateway Identity !=
Transport != Relay != Tracker Replay State\`.

### 1.1 User authorization belongs to the user

User rights are **not bound to one gateway**.

A user may use Gateway-A today and Gateway-B tomorrow without acquiring a new
user authorization merely because the transport endpoint changed.

The backend is the canonical owner of user authorization. When Internet is
available, the application refreshes/synchronizes the user's authorization from
the backend.

For offline use, the application may carry the last backend-issued/verifiable
authorization state so the same user can use any otherwise eligible enrolled
gateway. The exact proof, validity policy and stale-authorization bound are not
yet frozen.

Tracker firmware does **not** maintain user lists, phone identities or backend
account ACLs.

### 1.2 Gateway enrollment belongs to the gateway

A gateway must be enrolled/authorized by the backend before it may originate
trusted tracker commands.

Gateway enrollment is independent from the human user's rights.

Normal local access therefore requires both:

1. the user is allowed to request the operation; and
2. the gateway is currently enrolled and has sufficient cryptographic device
   capability/scope for that command family.

A gateway's maximum command capability must be cryptographically bounded by the
backend-issued gateway authorization. UI checks alone are not sufficient.

### 1.3 Backend signature is not required on every LoRa command frame

The backend establishes/refreshes gateway authorization. A normal offline command
frame need not carry a large backend signature on every packet.

Every command frame must still be cryptographically authenticated under a
reviewed gateway-to-tracker security context and must have replay protection.

The exact mechanism is a later secure-envelope decision. A leading candidate is
backend-derived per-(tracker, gateway, grant-generation) key material or an
equivalent standards-based delegation construction. This candidate is **not**
wire-frozen by this document.

## 2. Offline store-forward is a required behavior

The accepted product behavior is:

~~~text
10:00  User gives command in the application
10:00  Gateway creates the final secure command
10:00  Gateway/relay keeps it pending

10:37  Tracker wakes and sends its normal uplink
10:37  Relay sees that Tracker is awake
10:37  Relay transmits the already-stored secure command in the RX window
10:37  Tracker authenticates, replay-checks and applies it
~~~

The tracker uplink is therefore primarily a **delivery opportunity / wake
indication** for the stored command. It must not be mandatory to recreate the
command cryptography at the last hop.

This requirement applies even when Internet disappeared after the command was
created.

### 2.1 Meaning of a self-contained secure command

"Self-contained" means that, after the enrolled gateway creates the command
packet, an opaque relay can retain and forward the same protected packet later
without:

- contacting the backend;
- contacting the originating gateway;
- knowing a tracker root secret;
- decrypting or modifying the protected command;
- rebuilding the packet from a fresh rendezvous challenge.

Conceptually the protected object needs to bind at least:

~~~text
target tracker
gateway / issuer identity or security context
gateway sender sequence / replay value
command_id
command family + payload
gateway authorization context / generation as required
authenticated encryption / integrity tag
~~~

This is a conceptual list, **not a byte-layout freeze**.

## 3. Relay ownership

A normal relay remains opaque transport.

It may:

- retain ciphertext;
- deduplicate transport copies;
- observe routing metadata needed to know the target;
- notice a tracker uplink / RX opportunity;
- forward the stored protected frame;
- keep the frame pending until an authenticated completion/result or bounded
  transport-retention policy removes it.

It must not become the user authority, gateway enrollment authority or holder of
tracker root credentials merely because it stores/forwards a command.

If a relay misses the completion result and retries an old command, tracker replay
and application idempotency must prevent unsafe re-execution.

## 4. Required replay model for offline gateway-originated store-forward

A purely RAM rendezvous model is insufficient for the approved behavior because a
command may remain in a relay for minutes or hours.

Therefore the final design must provide durable replay protection for
gateway-originated self-contained commands.

Current direction:

- gateway sender state must never reuse a nonce/security sequence under the same
  key context;
- tracker must remember enough durable issuer replay state to reject already
  accepted gateway frames after reset/power loss;
- because gateway count per site is expected to be small, a **bounded small
  number of gateway replay/security contexts** is acceptable if required;
- the exact bound (for example 4/8/16), eviction policy and SecurityStore format
  are deliberately unfrozen;
- silently evicting a replay HWM is not acceptable if doing so could make old
  commands valid again.

This deliberately prefers a small bounded amount of tracker security state over
breaking offline relay/store-forward behavior.

M7P6F remains useful for the existing backend/asynchronous A2D replay path. A
future local-gateway durable replay design must be reviewed as a separate schema
and security change rather than silently repurposing M7P6F v2.

## 5. Command lifecycle and application idempotency

Security replay and logical command idempotency are separate.

Example:

~~~text
security sequence 381 + command_id ABC123
security sequence 382 + command_id ABC123   # retry of same logical request
~~~

The security sequence answers:

> Is this protected frame new for this issuer/security context?

The \`command_id\` answers:

> Has this logical operation already been applied?

For read-only operations, repeat execution may be harmless. For configuration,
geofence, actuation or other side-effecting commands, reset-safe idempotency or
version/CAS semantics must be designed explicitly.

\`TX_DONE\` is not success.

The UI may show "Uygulandı / Başarılı" only after an authenticated tracker RESULT
confirms application-level completion.

## 6. Internet-present behavior

When Internet is available:

- the application refreshes current user authorization from the backend;
- the backend remains the canonical user-authorization owner;
- gateway enrollment/refresh/revoke comes from the backend;
- backend-originated secure commands may continue to use the M7P6F durable A2D
  authority path and opaque relay/store-forward;
- the system may choose backend-originated or enrolled-gateway-originated command
  production according to the later secure-command design, but tracker must not
  need to know whether "Internet exists".

Tracker behavior must not depend on Internet availability.

## 7. Internet-absent behavior

If the user and gateway have the required previously established authorization:

~~~text
Application
    │  cached/verifiable user authorization
    ▼
Enrolled Gateway
    │  creates final protected command
    ▼
Relay(s)
    │  may retain it
    ▼
Tracker wakes
    │
    ▼
pending protected command delivered
~~~

The user may leave after placing the command. The originating gateway may also no
longer be RF-adjacent to the tracker once an opaque relay has custody of the
protected command.

This is a required product behavior, not an optional later optimization.

## 8. New gateway enrollment

New gateway enrollment may require Internet/backend access. That availability
tradeoff is owner-approved.

Normal target UX:

~~~text
ORUN application
→ add new gateway
→ backend enrollment / commissioning
→ gateway receives new grant/security generation
→ ready
~~~

The preferred design should avoid requiring:

- collection of all trackers;
- tracker-by-tracker USB work;
- firmware reload;
- O(fleet-size) ADD_GATEWAY RF propagation merely to introduce the new gateway.

The exact mechanism that lets a tracker validate a newly enrolled gateway is not
yet frozen. Per-tracker derived delegation is a leading candidate because the
backend and tracker already have tracker-specific security context, but this must
be audited before implementation.

## 9. Factory reset

Factory reset of a gateway means the gateway is **no longer ORUN-enrolled**.

It clears at least:

- operational grant/delegation material;
- active security/session material;
- operational sender/replay state;
- user-configured local settings;
- user PIN, returning local access to the factory-PIN state.

After factory reset the gateway must not resume by reusing:

~~~text
old key + counter reset to zero
~~~

Re-enrollment through the backend must create a fresh security/grant generation.

If all gateways are factory-reset and no Internet/backend is available, offline
local command service may remain unavailable until Internet returns. No complex
offline master-recovery mechanism is required for this edge case.

## 10. PIN and local human access

PIN protects **human → gateway local management/access**, not
gateway → tracker cryptographic authority.

PIN is required for reviewed local management paths such as:

- BLE;
- USB/cabled management;
- local Wi-Fi management.

Factory reset returns to the factory PIN. On first use, the application should
recommend changing it.

The security design should prefer a per-device unique factory PIN/label/QR secret
rather than one fleet-wide default PIN. Exact PIN length and UX are unfrozen.

Required properties include rate limiting/backoff and no derivation of tracker
security keys directly from the PIN.

## 11. User authorization refresh and offline residual risk

When Internet is available, the application refreshes user authorization from the
backend.

When Internet is absent, an already-authorized user may continue with the last
accepted offline authorization proof/policy.

Therefore immediate central revocation cannot be guaranteed for a completely
offline phone/gateway. This is an explicit residual risk of offline operation.

A later design must bound this risk without assuming a trustworthy tracker RTC or
GNSS time. The exact lifetime/generation mechanism is intentionally unfrozen.

## 12. What is superseded from the exploration report

The exploration report's \`H = A + D0\` recommendation intentionally gave up
long-lived offline gateway-originated relay/store-forward.

The owner **did not accept that tradeoff**.

The current owner-approved requirement is:

~~~text
Internet present:
    backend-originated command may store-forward

Internet absent:
    already-enrolled gateway-originated command MUST ALSO be able to
    store-forward through opaque relays and wait for a sleepy tracker
~~~

The accepted availability tradeoff is instead:

~~~text
first/new/re-enrollment of a gateway may require Internet/backend
~~~

Do not reintroduce the D0 "same live RX window only" limitation as if it were an
owner decision.

## 13. Security invariants to preserve

1. Backend is canonical owner of user authorization and gateway enrollment.
2. User authorization follows the user, not a particular gateway.
3. Gateway enrollment follows the gateway, not a particular user.
4. Tracker does not maintain user/phone ACLs.
5. Relay/gateway transport is not automatically user authority.
6. An explicitly enrolled command-capable gateway may hold narrowly scoped
   delegated command security material; this is an explicit exception to the
   default opaque-gateway rule and must not leak into ordinary relays.
7. No fleet-wide/group authentication secret.
8. Every protected issuer context must preserve nonce uniqueness and replay
   protection across reset/power loss as required by its security model.
9. Capability/scope limits must be cryptographically bound, not UI-only.
10. Replay != idempotency != delivery != RESULT.
11. Unauthenticated traffic must not cause durable security-state mutation.
12. Factory reset destroys ORUN gateway authorization and requires fresh backend
    enrollment.
13. Tracker Internet awareness is not part of the model.
14. TLP v1 bytes remain untouched until an explicit protocol cutover.
15. No claim of physical validation follows from this documentation decision.

## 14. Items deliberately left for the next design slice

The next security-command design must resolve, with tests and independent review:

- exact gateway credential/delegation construction;
- exact gateway sender nonce/counter ownership and reserve-ahead behavior;
- bounded tracker replay-context representation and safe reclamation;
- gateway revoke/rotation without unsafe stale-HWM loss;
- offline user authorization proof and stale-revoke bound;
- relay dedupe/retention/RESULT cleanup semantics;
- command expiry without relying on wall-clock time;
- reset-safe application idempotency for side-effecting commands;
- gateway secure-storage ownership and backup/rollback behavior;
- exact secure-envelope bytes, AAD and MTU/airtime budget;
- mixed-firmware fail-closed behavior;
- RF capacity at 10/100/1000 trackers;
- physical sleepy-tracker + relay store-forward validation.

No SecurityStore v2 or production-runtime change is authorized until those items
are explicitly sliced and reviewed.
