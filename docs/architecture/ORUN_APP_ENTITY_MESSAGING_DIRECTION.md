# ORUN Application Entity, Messaging and Offline Sync Direction

Status: **OWNER-APPROVED PRODUCT/ARCHITECTURE DIRECTION — documentation only.**

Baseline: `main@46d7a933f63d42d84fb386035be1c03af1d0c3c4`.
First recorded during M7P7C design review. This document does **not** implement
backend, Android, MESSAGE, Entity Registry, offline synchronization, secure RF,
new wire bytes, provisioning or authorization.

This record exists so product/application decisions do not become accidental BLE
milestone invariants. M7P7C owns the BLE application transport boundary; this
document owns the later application/entity/messaging direction.

## 1. Scope and invariants

Keep these separate:

```text
Device Identity
!= User Identity
!= Entity Identity
!= Entity Category
!= Capability
!= Enabled Service
!= Transport
!= Location Source
!= Authorization
```

A real-world entity/category such as animal, person, vehicle, gateway, valve or
sensor is application data. It is not the legacy firmware Role enum and must not
be inferred from BLE connection, hardware presence or a profile.

## 2. Entity Registry ownership

The backend is the Entity Registry **system of record and convergence point**.
It is not a runtime prerequisite for local field operation.

Authorized phones/local clients may keep a bounded offline cache and may create
permitted offline registry operations for later synchronization. A compatible
gateway may cache the minimum registry data needed for authorized local service,
but it is not the canonical owner and does not become a user/security authority.

Device firmware owns technical facts it can actually know:

- immutable/stable device identity;
- detected capabilities and capability health;
- enabled/effective services;
- observations and technical state.

The Entity Registry owns application facts:

- stable `entity_id`;
- display name;
- entity category;
- source/device-resource binding;
- visibility/ownership metadata;
- UI metadata.

UI-only metadata such as "cow", "İnek 27" or an emoji must not be repeated over
LoRa merely for presentation.

The Entity Registry must be exportable/backed up when the backend is introduced.
Losing one backend database must not make historical observations permanently
unattributable.

Registry permissions are application visibility/ownership policy. They are not a
substitute for device-side cryptographic authorization of protected commands.

## 3. Binding key and historical attribution

A binding is not assumed to be forever one whole physical device.

The model must support a **device resource / instance** when real hardware needs
it, for example one ORUN node exposing more than one sensor or actuator channel.
The initial tracker case may use an implicit instance `0`; no generic instance
framework is required in firmware until a real device needs it.

At minimum a binding record needs two different notions of time:

- **valid time** — when the real-world assignment was intended to apply;
- **record/audit time** — when ORUN learned/recorded the assignment, by whom,
  and what prior assignment it supersedes/corrects.

This permits "the collar was moved yesterday" or later correction without
rewriting the original observation stream.

Observations remain canonically tied to their technical source identity plus
observation time/time quality. Entity attribution is resolved from the binding
history. A backend may materialize/cache current views for performance, but must
retain source provenance and must not destructively rewrite old observations so
that a later rebind changes history.

`entity_id` is stable and is not silently reused for a different real-world
subject. Archive/merge/alias semantics, if required, remain later backend work.

No history-sequence boundary field is frozen here. Existing HistoryStore ticket/
sequence semantics are not repurposed as an Entity Registry clock.

## 4. Offline registry operations and conflicts

Do not add a CRDT library or device-side binding state for this first product.

A later backend/app implementation may use a small append-only operation log with
stable `op_id`, actor/installation identity and `base_revision`, for example:

```text
CREATE_ENTITY
UPDATE_METADATA
BIND
UNBIND
CORRECT / SUPERSEDE
ARCHIVE / MERGE
```

The exact storage schema is not frozen.

Conflict policy must depend on what is being changed:

- disjoint metadata fields may merge;
- a stale offline write to the same metadata field must not silently overwrite a
  newer authoritative edit merely because it arrived later;
- overlapping incompatible bindings for the same source/resource are
  `CONTESTED` until a deterministic, audited resolution is accepted;
- a source/resource has at most one effective entity binding at one instant,
  while one entity may legitimately have multiple technical sources.

For low-risk tracking/display entities, the UI may show a deterministic temporary
view while marking disputed attribution as uncertain and preserving the audit
trail.

For actuators or another safety-relevant binding, an offline rebind must not
silently authorize control of a different physical target. A contested/pending
actuator binding blocks protected actuation until the exact device/resource is
confirmed under the later authorization design. Protected commands are addressed
to the exact technical target, not only to a cached display name.

ACL/permission **mutation** does not need offline multi-writer support in v0.
Offline local operation may later use bounded cached/signed authorization grants,
but that is a separate security design and must not be inferred from stale UI
permissions.

Client wall-clock time is evidence with explicit quality, not security authority.
Do not resolve authorization or conflicts solely by trusting a phone timestamp.

## 5. Person/user location privacy

Person location is user/application data even when its source is a physical ORUN
device rather than the phone GNSS.

The personal-location sharing feature requires an explicit authorization/consent
state and visibility policy for the person being represented. Binding a hardware
tracker to a PERSON entity must not silently bypass the privacy controls that
apply when the same person's location originates from a phone.

Freshness/age is explicit and stale last-known location is never shown as live.
Exact retention and legal/compliance policy remain product/backend work for the
deployment jurisdiction.

## 6. Map/UI direction

One map may render animals, people, vehicles, gateways, actuators/valves and
sensors with distinct concise markers and filters.

The map is an overview, not a diagnostic dump. A tap may show a compact card;
entity-specific detail views own richer history such as:

- person/animal route;
- actuator command/operation history and reported physical state;
- sensor time-series;
- gateway/service diagnostics.

Category/icon is Entity Registry/application metadata. Capability/service state
still controls whether an action is actually available.

## 7. MESSAGE identity and recipient semantics

MESSAGE is a transport-independent application service.

A logical message keeps one stable `message_id` across Internet, BLE, gateway
custody, LoRa retry/fallback and store-and-forward. Message identity, secure
envelope nonce/counter state and transport dedupe are separate mechanisms.

For v1 application semantics:

- recipient is a user/account or explicitly defined recipient scope;
- an endpoint is one authorized application installation for that recipient;
- `DELIVERED` means at least one authorized recipient endpoint authenticated
  the message, successfully decrypted/validated it, durably stored it locally
  before expiry, and returned an authenticated delivery receipt;
- backend custody, gateway custody, BLE buffer receipt and RF `TX_DONE` are not
  `DELIVERED`;
- MESSAGE v1 has no human read receipt.

Other authorized installations may later fetch the same logical message and
deduplicate by `message_id`; v1 does not require every installation to receipt
before the sender sees `DELIVERED`.

Exact receipt bytes/MAC/tag format are not frozen here.

## 8. MESSAGE path selection

Normal MESSAGE routing is single-path first; do not consume Internet and LoRa in
parallel without a separately reviewed reliability class.

The **current custodian** of the logical message selects the next route from
fresh reachability state.

Owner-approved baseline preference remains:

1. use a currently confirmed Internet-backed delivery path when available;
2. otherwise use an available ORUN/LoRa path;
3. otherwise retain the message under bounded store-and-forward until a route
   appears or expiry ends it.

A direct recipient app session is one Internet-backed path. A future
backend-to-local-gateway-to-BLE path may avoid RF even when the recipient phone
itself has no Internet; whether it is admitted before LoRa is a later route-policy
decision and is **not frozen by this record**.

For ordinary MESSAGE, route failure or a bounded delivery timeout may trigger a
fallback while retaining the same `message_id`. "Emergency dual-path now" is a
separate future reliability class, not implicit MESSAGE v1 behavior.

## 9. Presence / reachability

Internet presence is not inferred from a Wi-Fi/mobile-data icon or push-token
existence.

A later Android/backend implementation should treat reachability as
**per-installation**, authenticated, server-timed and short-lived, using session/
check-in/lease evidence. Push may wake an app, but push delivery by itself is not
presence and is not delivery.

Separate reachability may later be required for "this recipient endpoint is
attached locally to gateway X". That is distinct from Internet presence and does
not by itself prove an RF route between arbitrary gateways.

Mobile OS background restrictions make every lease a hint, not a delivery fact.
Only the authenticated endpoint receipt closes `DELIVERED`.

## 10. Private-message confidentiality and threat boundary

Normal gateway/relay infrastructure remains opaque custody/transport and does not
need private MESSAGE plaintext.

The product target is also that ordinary backend message storage/routing need not
see private MESSAGE plaintext. This requires message-level end-to-end protection;
it is separate from the device secure-RF envelope.

Do not overclaim the threat model. If the backend is the only endpoint-key
directory, a malicious/compromised backend could potentially substitute a false
endpoint key unless the later design adds an independently verifiable
key/identity mechanism. Multi-device key distribution, phone loss, history
recovery/backup and group messaging therefore need focused later design.

Do not invent group cryptography. If group messaging is implemented later, use a
reviewed standard appropriate to the product and RF constraints.

Private MESSAGE and system/safety EVENT are separate application classes. If a
backend escalation service needs selected event metadata, design that event
visibility explicitly rather than weakening private-message confidentiality.

## 11. LoRa MESSAGE prerequisites and airtime

Do not implement LoRa MESSAGE merely because the application semantics are
defined.

Its runtime depends on later reviewed prerequisites including:

- authenticated secure-envelope receive/transmit;
- downlink/rendezvous behavior for sleepy nodes;
- recipient/gateway local attachment/bridge behavior;
- bounded mailbox/store-forward ownership;
- airtime admission, priority and retry policy;
- explicit mixed-fleet/new-wire rules.

MESSAGE traffic must not starve alarms, critical command results or current
tracking.

Capacity modelling must use the repository airtime estimator and model at least:

- the current 3-minute development tracking default;
- intended normal product intervals such as 15–30 minutes where applicable;
- burst cases (geofence/event convergence, reboot/backlog);
- different relay fan-out values rather than assuming every tracker is heard by
  a fixed number of relays.

Regulatory duty-cycle/ERP rules are deployment-jurisdiction inputs and must be
verified separately. Do not freeze a percentage from a generic EU example into
firmware or product capacity claims.

A small host-side capacity model is appropriate before a 50–100-device field
claim. Firmware airtime instrumentation may be added only in a separate bounded
diagnostic slice if existing counters are insufficient; it must not change RF
behavior merely to collect evidence.

## 12. Supersession of historical/proposed V1 wording

`ORUN_SYSTEM_ARCHITECTURE_V1.md` is a historical/proposed architecture audit,
not the current source of truth.

For current application direction, this document supersedes its older wording
where it differs:

- MESSAGE v1 does not include a read receipt;
- animal/person/vehicle map category/icon is Entity Registry metadata rather
  than a firmware capability/profile identity;
- real-world assignment history is owned by the Entity Registry/binding model
  described above.

The historical file itself is not rewritten to pretend those conclusions were
part of its original audit.
