# ORUN Field RF, Coverage Learning and Diagnostics Plan

Status: **OWNER-APPROVED PRODUCT/ARCHITECTURE DIRECTION — documentation only.**
No new RF channel plan, multi-hop behavior, backend coverage service, BLE diagnostic
service or runtime change is authorized by this document alone.

Baseline: `main@679f145ab7576eef3216613a826e05f8ad40876f`.

This document records field-network and serviceability decisions that must survive beyond
conversation history.

## 1. Product goal

Infrastructure deployed in one pasture/site should become useful shared ORUN coverage
where RF geometry permits. A relay/gateway installed for one project/customer must not
be artificially prevented from helping another eligible ORUN node merely because the
backend ownership/project differs.

At the same time, scaling work must follow measured need. ORUN is not implementing a
300-device scheduler today just because the architecture should not block future growth.

Current practical development scale remains small (roughly 3–30 devices). Larger numbers
are a future capacity/field-validation concern.

## 2. RF domain is not customer/project identity

Keep these separate:

```text
RF channel/domain != customer/project
RF channel/domain != DeviceIdentity
RF channel/domain != profile
RF channel/domain != security credential
RF channel/domain != user ownership
```

Customer/project separation belongs to backend ownership/security.

A channel/frequency plan is a **capacity/interference resource**, not a security boundary.

Do not make "Project A = Channel A" the product architecture. That would strand useful
relay/gateway infrastructure and create unnecessary per-project hardware cost.

## 3. Regional/common RF domain direction

Nodes intended to share relay/gateway coverage should normally use a compatible regional
RF domain so that a device moving from one pasture/area can benefit from infrastructure
placed in another nearby area.

Example product intent:

```text
Pasture A -- Relay A ---- overlap ---- Relay B -- Pasture B
     tracker/person may move and use whichever eligible infrastructure is in range
```

This is a product direction, not a claim that current TLP v1 supports arbitrary multi-hop.

Current frozen TLP v1 still has exactly one RF relay hop and rejects nested
`RELAY_FORWARD`. Any future bounded multi-hop/cell bridge requires its own protocol,
dedupe, airtime, security and mixed-fleet review.

## 4. Cellular/spatial-reuse planning concept

Long-term field planning may resemble a simple cellular network:

- coverage cells overlap enough that moving nodes do not fall into intentional dead gaps;
- not every node must hear every other node;
- physically separated cells may reuse the same RF resource with less mutual collision;
- local relay/gateway placement provides continuity between useful coverage areas.

Do **not** deliberately create blind areas just to increase capacity.

The goal is:

> continuous useful coverage where required, while avoiding one unnecessarily huge
> collision domain.

Terrain, antenna placement/height, propagation and actual RF observations determine cell
geometry. A channel number alone does not create a smaller physical coverage area.

## 5. Scaling path

Scale in this order:

1. keep the tracker/device protocol and ownership model simple;
2. improve relay/gateway placement from real field evidence;
3. add gateways where coverage/capacity requires them;
4. use spatial reuse naturally created by geography/terrain;
5. only when measured airtime/interference requires it, introduce additional RF
   channels/domains;
6. if needed, use a multi-channel LoRa concentrator-class gateway so one infrastructure
   point can hear several channels simultaneously.

Do not implement today:

- speculative SX130x/concentrator HAL support;
- dynamic channel hopping framework;
- per-customer RF allocator;
- gateway clustering;
- multi-channel tracker scheduler.

A future multi-channel gateway is an infrastructure evolution, not a reason to entangle
today's tracker identity/profile with channel assignment.

## 6. Capacity metric that matters

"Total fleet size" is not the only useful capacity number.

The important field metric is the amount of active traffic inside the same effective RF
collision domain, including relay retransmissions.

Two far-apart groups on the same nominal frequency may transmit concurrently if they
cannot materially interfere with the same receivers. Conversely, repeated relay hops
consume additional airtime.

Future capacity work should therefore measure:

- active transmitters per local collision domain;
- packet airtime by frame class;
- relay-forward count/hop behavior;
- collision/loss/duplicate rates;
- gateway/relay receive load;
- duty-cycle/regulatory constraints;
- alarm/backlog traffic, not only normal tracking interval.

Do not promise a fixed "N devices" limit without these field assumptions.

## 7. Coverage learning remains a product requirement

The earlier coverage-learning direction remains valid.

ORUN should learn real coverage over time from actual reception observations instead of
pretending RSSI maps directly to distance.

Useful observation metadata includes, when available:

```text
origin device
accepted device position/time
receiving gateway/relay
direct vs relay path
RSSI
SNR
reception time
RF configuration/version
duplicate reception observations
```

The backend can combine repeated observations into a coverage dataset/heatmap.

Important rules:

- RSSI/SNR are link observations, not metres.
- "No observation" is not automatically "no coverage"; it may mean no device travelled
  there or no sample was scheduled.
- Distinguish unknown/insufficient-data areas from repeatedly observed weak/dead areas.
- A relay-path observation should not be displayed as if it were a direct gateway link.
- Device timestamp/location freshness and quality must be considered before using a
  sample for mapping.
- Coverage data is mainly backend-owned; do not continuously write it into tracker flash.

Product fruit:

- show strong/weak/unknown areas;
- identify where direct coverage is poor but relay coverage works;
- identify genuinely useful relay/gateway placement candidates;
- compare before/after field infrastructure changes.

No coverage backend/map is implemented today.

## 8. Current diagnostic visibility

Current firmware already provides useful USB Serial diagnostics; this is not a future
claim.

Examples currently present include:

- firmware/build/reset reason and watchdog indication;
- storage recovery/count/corruption/backlog information;
- accelerator presence/fault and bounded activity diagnostics;
- radio listen policy/state/window/sleep counters via `RADIO?`;
- TX position traces;
- RX RSSI/SNR;
- DIRECT vs RELAY receive path;
- relay duplicate/queue-drop/nested-reject events;
- malformed packet and RX error reporting.

These logs are valuable service evidence but are not yet a coherent product diagnostic
API/UI.

## 9. Target Health/Diagnostics ownership

Create one bounded, transport-neutral Health/Diagnostics owner when a real consumer needs
it.

Do not make USB Serial itself the owner of diagnostic truth.

Target shape:

```text
subsystems -> bounded Health/Diagnostics state
                     |
                     +-- USB diagnostic transport
                     +-- BLE diagnostic transport (later)
                     +-- backend health summary (later)
```

The useful diagnostic data model is intentionally small:

### STATUS SNAPSHOT

Current subsystem state/reason, e.g. GNSS, radio, storage, security, accelerometer,
battery/firmware where available.

### COUNTERS

Bounded fault/drop/reset/recovery counters useful for diagnosis.

### RECENT EVENTS

A small bounded recent-event ring for important transitions such as fix/store/TX/RX/error.

Do not create an unlimited logging framework or persist every diagnostic line to flash.

A recent-event buffer should normally be RAM-only; only facts that already require durable
ownership (for example reset reason or a store's durable state) should be durable for
their own reason.

## 10. BLE diagnostics direction

Future BLE serviceability should expose structured diagnostics, not simply mirror an
unbounded Serial text stream.

Tracker policy remains:

- BLE normally off;
- explicit maintenance/admission window approximately 10 minutes;
- if no authenticated maintenance operation starts, BLE closes;
- connection alone is not enough to keep it open;
- an authenticated maintenance operation may keep the session available through bounded
  completion;
- a future stalled-session watchdog is required.

Gateway/mobile-gateway profiles may keep BLE available when their availability/power
contract allows it.

BLE transport must not own subsystem state. The same diagnostic facts should be readable
over USB or BLE without duplicating business logic.

Diagnostics can expose sensitive information (identity, last location, RF neighbours,
firmware/security health). BLE connection/bonding alone does not grant access; protected
diagnostic access requires the reviewed application security/authorization path.

BLE runtime/admission remains M7P7 scope; this document does not enable it.

## 11. App/backend serviceability

Normal users should see useful product outcomes, not raw engineering logs.

Examples:

```text
Device online/offline
GNSS OK / degraded
Radio link good / weak / unknown
Last seen
Direct / relay path
Battery estimate
Service warning
```

An authorized service/advanced view may expose the deeper counters/events needed for
field diagnosis.

Detailed user/account permissions remain backend-owned. The application should hide
features the logged-in user is not authorized to use. Device firmware must still verify
the cryptographic authority of protected operations; UI hiding is not the security
boundary.

## 12. Ownership summary

```text
Tracker/device
- raw local subsystem observations and bounded diagnostics
- no long-term coverage database
- no customer/project RF ownership logic

Relay/gateway
- reception/path/RSSI/SNR metadata
- shared infrastructure where policy/security permits
- opaque payload handling by default

Backend
- users/owners/permissions
- long-term reception observations
- coverage learning/heatmap
- fleet/cell capacity analytics

App
- authorized UI
- service/diagnostic presentation
- BLE/USB/backend bridge as implemented
```

## 13. Explicit non-claims

This plan does not claim:

- current multi-hop routing;
- current multi-channel/concentrator support;
- a 300-device tested RF domain;
- an implemented coverage heatmap;
- BLE diagnostics today;
- RF range in kilometres;
- physical coverage/capacity without field measurements.

The current one-hop TLP v1 behavior and all physically proven M0–M6 behavior remain
unchanged until an explicit implementation milestone authorizes a change.
