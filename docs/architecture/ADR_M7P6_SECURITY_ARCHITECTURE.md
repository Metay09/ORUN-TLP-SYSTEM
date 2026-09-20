# ADR: M7P6 Security Architecture Direction

Status: **OWNER-APPROVED SECURITY DIRECTION. M7P6A is the design record; M7P6B
SecurityStore + TX nonce persistence is now implemented and software/build validated.
No secure RF envelope, provisioning transport, BLE runtime, command path or TLP v1
protocol-byte change is authorized by this ADR alone.**

Baseline for M7P6A design: `main@679f145ab7576eef3216613a826e05f8ad40876f`
(M7P5 merged).

M7P6B implementation baseline:
`main@003a891a2b6e66c267e68cee2e704860f97bed69`.

M7P6B final implementation SHA before docs-only closeout:
`6d3009d42d9fb36026be5379171d8994a71dbaf6`.
See `docs/milestones/M7P6B.md` for the exact on-flash format, fault-injection,
wear arithmetic and validation evidence.

This ADR records the owner-approved synthesis reached after an independent security
architecture review. It exists so future Claude/Astra/Codex sessions do not have to
recover security decisions from conversation history.

It refines, but does not silently rewrite, the persistence allocation already decided
in `ADR_M7_PERSISTENCE_LAYOUT.md`:

```text
0x0E7000..0x0E9000  Security + anti-replay (M7P6)
0x0E9000..0x0EB000  Durable ConfigStore (M7P5)
0x0EB000..0x0ED000  BLE bonds/InternalFS (M7P4)
0x0ED000..0x0F4000  History
```

TLP v1 bytes remain frozen.

## 1. Product/security objective

M7P6 establishes the durable security foundation needed before private location,
authenticated delivery/contact, remote configuration, messaging or actuation can
be trusted.

The design must preserve these boundaries:

- Device Identity != security credential != user identity.
- Gateway/relay transport != trusted application authority.
- TX completion != delivery/contact.
- History sequence/tickets != security nonce/counter state.
- Backend user authorization != device-side cryptographic verification.
- BLE bond != application authorization.

Do not invent cryptography.

## 2. Trust model

Default gateway and relay behavior is **opaque custody/forwarding**.

A normal relay/gateway may:

- receive an eligible ORUN frame;
- attach its own reception metadata such as time/RSSI/SNR;
- deduplicate for transport efficiency;
- store ciphertext;
- forward ciphertext.

A normal relay/gateway must not, merely because it is a gateway/relay:

- possess tracker root keys;
- decrypt private tracker payloads;
- forge tracker-originated traffic;
- create a trusted delivery ACK;
- become the origin identity;
- become the user/owner authority.

This keeps infrastructure shareable: a relay or gateway installed for one field/customer
may help another eligible ORUN device in RF range without learning that device's
private payload.

A future explicit site-local trusted endpoint may be designed separately if the product
needs offline local decryption/alarms. It is not the default gateway trust model.

## 3. Device identity and security credential

The current `DeviceIdentity`/legacy uint64 remains a stable public lookup identity and
compatibility surface. It is **not** authentication evidence.

The approved security direction is one independent random root credential per device:

```text
DeviceIdentity       public logical device identity
credential_id        random identifier for one security/provisioning lifetime
key_epoch            key/rotation generation within that credential
K_root               random 256-bit root secret
TX reservation       nonce-safety counter state
```

### 3.1 credential_id

A durable random `credential_id` is required so a physical device can be securely
reset/re-provisioned without confusing its new security lifetime with its old one.

Example:

```text
same DeviceIdentity
old credential_id=A, K_root=K1, epoch=1
security reset/re-provision
new credential_id=B, K_root=K2, epoch=1
```

M7P6B freezes `credential_id` at **128 random bits (16 bytes)**. Re-provisioning
must create a new credential lifetime. The store also refuses immediate reuse of the
currently active `credential_id` or currently active `K_root` when resetting the TX
counter to zero; preventing reuse of older historical roots remains a provisioning-layer
responsibility.

### 3.2 No fleet/group authority key

Do not use a single fleet/network/group key as an authentication authority.

Compromise of one tracker must not expose or authorize the whole fleet.

Group/broadcast authorization, if ever required, needs its own reviewed design later.

## 4. Cryptographic direction, not yet a wire-format freeze

The current standards-based direction is:

```text
random 256-bit K_root
        |
      HKDF-SHA256
        |
purpose/direction-specific traffic keys
        |
standards-based AEAD
```

AES-128-CCM with an 8-byte authentication tag is the current leading secure-envelope
candidate because it is standardized, compact enough for LoRa airtime and suitable for
nRF52840-class hardware/software support.

However M7P6 storage work must **not** prematurely freeze:

- the secure-envelope byte layout;
- nonce byte layout;
- exact HKDF label strings;
- key-ID encoding;
- counter encoding on air;
- AAD layout;
- final AEAD/tag choice.

Those are frozen only in the later secure-envelope milestone after pinned-library,
known-answer, airtime and hardware validation.

Regardless of AEAD choice, ORUN's invariant is simple:

> A nonce must never repeat under the same traffic key.

Do not design around the idea that nonce reuse is "less bad" for one AEAD than another.

## 5. SecurityStore ownership and minimum durable state

The M7P6 partition remains exactly `0x0E7000..0x0E9000`, physically separate from
ConfigStore, BLE bonds and History.

SecurityStore v1 now persists only state needed by the security foundation:

```text
CREDENTIAL
- format/schema metadata
- DeviceIdentity binding
- credential_id
- key_epoch
- K_root

TX_RESERVE
- credential_id / key_epoch binding
- absolute reserved TX counter bound
```

Do **not** pre-allocate speculative durable fields for:

- user IDs;
- owner IDs;
- phone lists;
- detailed permission tables;
- RX replay HWM;
- command IDs/results;
- BLE keys;
- QR/claim secrets;
- geofence security state;
- message state;
- firmware minimum-version state.

Those belong to later owners when the corresponding service/security path actually
exists.

BLE bond material remains owned by the M7P4 bond partition, not SecurityStore.

## 6. TX counter / nonce-safety persistence

The security TX counter is a separate namespace from the legacy TLP v1 sequence and
from HistoryStore's sequence/ticket reservation.

Its purpose is nonce uniqueness, not ordinary application ordering.

The design direction is block reservation:

```text
durably reserve an absolute future bound
        |
wait for durable completion/readback
        |
only then use counters from that reserved range
```

M7P6B freezes the initial reservation block at **256 counters** with an absolute
**exclusive** durable bound. This mirrors a useful existing reservation concept without
sharing its namespace or storage. A reboot skips to the last durable bound and reserves a
fresh block before any new protected TX counter may be returned. Counter exhaustion fails
closed; no rollover protocol is invented here.

After reset, unused reserved counters may be skipped. Wasting counters is acceptable;
reusing a nonce under the same key is not.

## 7. SecurityStore mutation pattern

A ConfigStore-style whole-page erase/rewrite for every counter reservation is rejected.

Counter reservations are frequent relative to credential rotation. The approved
direction is:

- page-level A/B ownership remains;
- small committed records append within the active security page;
- CREDENTIAL changes are rare;
- TX_RESERVE records append without erasing the page each time;
- compaction ping-pongs to the other page when needed;
- every record is power-cut safe using explicit commit-last semantics;
- recovery is reconstructed only from committed flash content;
- suspicious/torn state fails conservatively and must not permit counter rollback.

M7P6B freezes the exact v1 layout: 32-byte page header, one 68-byte CREDENTIAL slot and
111 36-byte TX_RESERVE slots per 4 KiB page. Record bodies/CRC are committed before their
record commit words. The **page-header commit word is additionally the A/B page activation
marker and is programmed last only after the complete new-page snapshot is durable**.
Therefore an interrupted compaction cannot make a higher-generation page authoritative
without its carried-forward nonce high-water mark.

Recovery is deliberately conservative: unsupported future-format pages block downgrade;
non-erased invalid reservation state, committed header corruption and impossible
append-log gaps fail protected security state closed rather than falling back to a lower
counter bound. See `docs/milestones/M7P6B.md` for the audited recovery rules.

### 7.1 FlashMutationGate priority refinement

Existing ADR priority for genuinely critical security durability remains valid.
M7P6B should distinguish:

- a short critical reservation/credential commit that is blocking a protected operation;
- security maintenance such as erase/compaction that can wait.

Security maintenance must not starve live store-before-send History work. Do not build a
generic scheduler; make the minimum bounded ownership change needed by actual M7P6B.

## 8. User/owner authorization belongs primarily to backend/app

The device must not become a user-account database.

Backend owns:

- user accounts;
- device ownership/account relationships;
- detailed permissions;
- phone/session/delegation lifecycle;
- revocation policy.

The application should display only capabilities/actions the logged-in user is authorized
to use. Hiding unauthorized controls is correct UX, but it is not by itself a device
security boundary.

A protected device operation must later independently verify that the cryptographic
authority presented to it is valid for that operation. The device does not need to know
the human's name/account details to do this.

Therefore no current SecurityStore user/owner/phone/permission list is authorized.

## 9. Later receive/command security ownership

These concepts are intentionally deferred, not cancelled:

### RX replay HWM

Added with the authenticated downlink/secure receive layer. It answers:

> Has this authenticated sender/counter already been accepted?

It is security/replay state, not application command state.

### command_id / result

Added with the command/application layer. It answers:

> Has this logical operation already been executed, even if retransmitted in a new valid
> secure frame?

A new secure counter does not make an imperative command safe to execute twice.

### permissions/delegation

Added with the authorization layer. Detailed human/user permissions remain backend-owned;
the device later needs only the minimum cryptographic authorization proof/class required
to safely accept protected operations.

No implementation of these three items belongs in M7P6 SecurityStore v1.

## 10. TLP v1 and secure protocol migration

TLP v1 remains byte-for-byte frozen and unauthenticated.

Do not:

- reinterpret reserved v1 bits as security;
- append a hidden MAC while calling the frame v1;
- weaken golden/compatibility fixtures;
- treat a gateway reception as authenticated origin/contact.

Future protected traffic must use an explicit new secure envelope/protocol version.

Mixed-fleet migration must be explicit: legacy v1 remains identifiable as legacy and
must never be silently upgraded to trusted/authenticated data by a newer gateway.

## 11. Provisioning and reset boundary

The following direction is approved:

- first credentials require explicit provisioning;
- security reset is distinct from normal config reset;
- security credential reset/re-provision is a physical-service-authorized operation in
  the first design; do not add an ordinary remote credential-reset path;
- normal config reset must not erase security credentials/counters;
- re-provisioning creates a new `credential_id`;
- keys must never appear in ordinary logs or config dumps;
- there must be no ordinary remote key read-back API.

The exact provisioning ceremony is **not yet frozen**.

In particular, this ADR does not authorize exporting `K_root` over USB or BLE merely
because an independent review suggested it. USB possession, BLE commissioning, backend
registration, key wrapping/escrow and second-phone delegation need a focused provisioning
design before implementation.

BLE provisioning belongs to M7P7 or a later focused slice after the required security
foundation exists.

## 12. Bootloader / DFU security remains UNKNOWN

Repository audit has verified the current serial upload tooling and `--singlebank`
client behavior, but the physically installed RAK4630/RAK4631 bootloader's exact
signature, rollback/bank and interrupted-update behavior remains **UNKNOWN**.

Do not claim the bootloader is signed or unsigned without physical/source evidence.

Also unresolved: whether every real update/DFU path preserves
`0x0E7000..0x0ED000` (Security, Config, Bonds) as intended.

Before storing production credentials or enabling high-risk remote actuation, relevant
update paths require physical sentinel/preservation tests and a reviewed firmware
authenticity story. BLE OTA/DFU validation remains M7P7/M7P8 scope when that path exists;
do not make a not-yet-implemented BLE OTA path an M7P6 completion prerequisite.

## 13. Milestone split

To keep the security milestone bounded:

### M7P6A — architecture/design

Documentation only:

- trust model;
- per-device root/credential lifetime;
- credential_id;
- no group authority key;
- opaque gateway default;
- nonce/counter durability rules;
- SecurityStore ownership;
- backend/app authorization ownership;
- TLP v1 migration boundary;
- explicit bootloader/DFU unknowns.

### M7P6B — SecurityStore + TX nonce persistence — IMPLEMENTED

The durable foundation is implemented at
`6d3009d42d9fb36026be5379171d8994a71dbaf6`:

- SecurityStore recovery/state;
- DeviceIdentity-bound credential lifetime;
- 128-bit credential_id + 256-bit K_root durable representation;
- append-style 256-counter TX reservation;
- activation-last A/B compaction and fail-closed recovery;
- bounded FlashMutationGate integration with
  `SEC_CRITICAL > History > Config > SEC_MAINT`;
- host fault-injection/property tests and RAK4630 build validation.

It does **not** introduce secure RF packets. Real SoftDevice-enabled async security flash,
electrical power-cut and production credential provisioning remain physically unvalidated.

### M7P6C — pinned CryptoCell primitive proof — VALIDATED

This bounded test-only slice validates the candidate HKDF-SHA256 and
AES-128-CCM primitives against published vectors on the RAK4630/RAK4631
reference platform before any secure-envelope bytes are frozen.

Initial owner hardware evidence passed RFC5869 HKDF-SHA256, RFC3610
AES-128-CCM encryption, valid authenticated decrypt and a one-bit wrong-tag
rejection/recovery check. The exact pinned `nrf_cc310_0.9.13-no-interrupts`
binary returned `CRYS_FATAL_ERROR` rather than the header's dedicated CCM
MAC-invalid code for that wrong-tag decrypt. M7P6C treats this only as a
version-pinned test compatibility observation; it does not authorize
production code to classify arbitrary `CRYS_FATAL_ERROR` as authentication
failure.

Independent review required broader negative-input and repeated-forgery
coverage before closing the slice. The expanded hardware image passed its
aggregate gate, which requires all 13 authenticated-input mutation cases and
all 1000 forged/valid recovery iterations to complete successfully. The probe
remains isolated and does not change TLP v1 or production packet paths.

A later production secure-envelope implementation must also resolve shared
CryptoCell ownership with Bluefruit/SoftDevice. Bluefruit initializes the same
global nRFCrypto/CC310 facility; the isolated M7P6C image does not prove
concurrent production use, and production ORUN code must not copy the probe's
`nRFCrypto.end()` cleanup pattern.

### Later secure-envelope milestone

Freeze and implement:

- AEAD/KDF library and vectors;
- secure envelope bytes/AAD/nonce encoding;
- authenticated uplink/downlink;
- backend/app verifier;
- mixed-fleet rules.

### Later command/authorization milestone

Add, only when needed:

- RX replay HWM;
- command IDs/results/idempotency;
- authorization/delegation;
- expiry/freshness;
- actuation-specific safety semantics.

### M7P7/M7P8

BLE runtime/admission, secure commissioning/maintenance transport and DFU validation,
without confusing BLE bonds with application authorization.

## 14. Explicit non-claims

This ADR does not mean any of the following is implemented or physically proven:

- production key generation/provisioning;
- AES-CCM/HKDF in the packet path;
- authenticated ACK/contact;
- secure commands;
- user authorization on-device;
- BLE secure maintenance;
- DFU preservation of the security partition;
- signed/rollback-capable bootloader;
- encrypted History;
- protection against physical key extraction.

Every future milestone must report software/build/physical evidence separately.
