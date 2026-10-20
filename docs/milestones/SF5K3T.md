# SF5K3T — Gateway grant persistence / RF release boundary (design gate)

**Status: DESIGN ONLY.** Parent: SF5K3S `50e1c8bb7dca885c357cc66e31100e5738569c9d`.
SF5K3S focused host, full host regression, and RAK4630 target build were
reported PASS from the user's Debian checkout on 2026-10-21. This SF5K3T
document has NO new execution evidence and is NOT a firmware release.

## Goal

Provide a production-trustworthy, persistent per-Tracker/per-Gateway
`GatewayCustodyGrantProvider` without exposing a Tracker's `K_root`,
overwriting any existing Flash owner, treating mere RF reception as authority,
or ever emitting a custody ACK before proven durable intake.

## Verified layout constraints (firmware/include/storage_config.h)

| Physical area | Extent | Existing owner / promise |
| --- | --- | --- |
| Product | `0xC5000..0xE5000` (128 KiB, 32 pages) | OBS1 ObservationStore **OR** OCQ1/OCQ2 CustodyStore, exclusively |
| Geofence | `0xE5000..0xE7000` (8 KiB) | GeofenceStore |
| Security | `0xE7000..0xE9000` (8 KiB) | SecurityStore v2/v3, credential, nonce/replay, policy |
| Config | `0xE9000..0xEB000` (8 KiB) | ConfigStore |
| BLE bonds | `0xEB000..0xED000` (8 KiB) | InternalFS/bonds |
| History | `0xED000..0xF4000` (28 KiB) | HistoryStore |

These areas abut with no spare reserved page. The firmware application
ceiling is `0xC5000`. DO NOT silently allocate a Gateway grant page from
product, security, config, geofence, history, bonds, application, or
bootloader memory. Never mount OCQ2 over OBS1/OCQ1; never erase an existing
owner as part of enrollment. A successful RAK4630 **build** does not prove a
safe on-device mount.

### Important dual-role constraint

`product_region::ownerFor({gateway=true, tracking=true})` presently chooses
Custody, not Observation. Consequently, a mobile node that performs both
Gateway and Tracker roles does NOT automatically have two durable product
stores. This must be resolved explicitly before dual-role activation.
Similarly, an existing Tracker SecurityStore v3 Gateway *allow-list*
(4 Gateway slots) is NOT a persistently enrolled Gateway-side database of
many Tracker-scoped custody keys. Do not reuse it by name alone.

## Security / custody invariants

1. **Provisioning origin:** Only a separately authenticated operator/Edge/
   enrollment service may install, rotate, or revoke grants. Untrusted LoRa,
   BLE, Tracker packet headers, a default-constructed token, and host test
   mock providers can never grant authority.
2. **Secret scope:** Gateway receives ONLY its authorized per-Tracker,
   per-Gateway, per-credential-epoch, per-generation `K_custody_ack`; it
   does not receive or derive the Tracker's `K_root`. Device identifiers,
   epoch, custody floor, generation, and key MUST bind to one authenticated
   durable enrollment identity.
3. **Power-loss safety:** No staged/partial, torn, stale, rolled-back,
   ambiguous, foreign-owner, or not-yet-readback-proven enrollment may be
   exposed by `activeGrant()`. Publishing success before durable commit
   and exact readback is forbidden. Reboot must reconstruct exactly the
   authoritative committed snapshot or fail closed.
4. **Revocation / TX:** The authoritative grant generation/floor is rechecked
   at ACK emission. Today, `takeAuthorizedAck()` rechecks only at ACK
   handoff; an ACK buffered for a later RF transmission can race revocation.
   The future loop-owned *privileged RF sender* must serialize revocation,
   last authority check, and actual TX admission (including queue/callback
   ownership). A queue insertion alone is not over-air delivery.
5. **Durability:** Local Gateway ACK requires exact object commit +
   readback via `ProductCustodyStore`, not TX_DONE, RAM caching or a
   transient receive interrupt. Signed Edge acceptance is a distinct
   future authority path and may not be impersonated by local reception.
6. **Verification:** Gateway intake of protected ciphertext is opaque; its
   admission does not prove the Tracker's payload AEAD was checked. The
   backend separately validates AEAD and stable logical observation identity.
7. **Key material:** No root/scoped key/logged plaintext in diagnostic
   output, return-status text, packet radio headers or Git source. Wipe
   short-lived key scratch. Key confidentiality at rest requires an explicit
   physical-threat and device-protection design; Flash CRC alone is never
   authentication.

## Persistence design decision pending implementation

Do NOT freeze a new on-Flash format yet. Before selecting a physical
provider, quantify the number of *simultaneously granted trackers* per
Gateway, the ability to operate offline, and the security model for stolen
hardware. An A/B crash-safe journal for even one grant must store the
scoped 32-byte key plus tracker/gateway IDs, credential epoch, floor,
generation, revision, commitment and authenticity data. A two-page 8 KiB
SecurityStore cannot be assumed to hold an unbounded herd or two independent
owners. Physical allocation must be backed by a reviewed capacity/retention
calculation, migration/rollback strategy and target-device qualification.

Candidate deployment modes to decide **separately**:
- Fixed standalone Gateway: native durable provisioning with explicit
  account of security partition and coexisting tracker duties; external
  trusted storage may be needed for fleet-scale offline service.
- Phone-hosted/mobile Gateway: protected, authenticated phone/Edge grants
  with a strictly bounded temporary device session; offline limitations and
  revocation latency must be specified. Do not label a phone's volatile
  session as persistent embedded Gateway enrollment.

Select one actual provisioning authority and threat model before introducing
production `activeGrant()` implementations. No "always allow" fallback.

## Next implementation slices

**T1 — Capacity / owner decision (NO FLASH MUTATION)**
- Document supported fixed/mobile/dual-role configurations and offline grant
  cardinality; inspect existing pages read-only and refuse foreign owners.
- Approve a non-overlapping durable storage strategy or authenticated external
  grant-store strategy. Explicitly decide behavior of a dual-role node whose
  product region is already OBS1.

**T2 — Authenticated persistent grant provider (HOST FIRST)**
- Implement schema/codec and authenticated grant import with precise
  per-tracker identity and key scope, monotonic revision/generation, revocation,
  corruption/torn-write/rollback rejection, fixed bounded capacity, commit-last
  and exact readback. Never export `K_root`.
- Real write owner must serialize with `FlashMutationGate` if physical flash
  is approved; no automatic migration or silent erase. Test reboot at every
  mutation boundary.

**T3 — Serialized authorized RF sender (HOST FIRST)**
- Compose current SF5K3S ingress, durable custody owner, newly approved grant
  provider and *privileged* TX owner on the application loop (not IRQ).
- Cover revocation after intake, after commit, after ACK handoff and before
  physical transmit; stale pending output must not escape.

**T4 — Hardware qualification (EXPLICIT SEPARATE RELEASE)**
- Full/focused host + sanitizers, pinned RAK4630 build, actual CC310 KAT,
  physical flash power-cut/readback tests, region census, two-device LoRa
  receive/ACK/retry, no ACK-before-durability, and duty-cycle/sleep bounds.
- Only after reviewed enrollment and rollback story: gated runtime opt-in,
  controlled deployment; no automatic `main` activation.

## Current release boundary

NO new firmware behavior in this design slice. No Gateway grant provisioning,
no live RF ACK transmission, no Tracker release on radio, no physical flash
erase or firmware upload. SF5K3S PASS proves host semantics + compile, not
field custody. Keep any implementation PR in DRAFT until the applicable
acceptance gates above have been executed.
