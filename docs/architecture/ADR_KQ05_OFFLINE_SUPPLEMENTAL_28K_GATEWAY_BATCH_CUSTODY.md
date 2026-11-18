# ADR (DRAFT): KQ05 offline-first selective Gateway custody ACK + per-device 28 KiB supplemental logs

**Date:** 2026-11-18  
**Status:** PROPOSED — source-level decision only; NOT implemented or field approved  
**Base:** `98f56f22338eb8dcb16baa5346a729098022fe3e` (KQ05 host RF-policy branch)  
**Supersedes candidate implementation direction:** PR #333 / #334 (2-page / 8 KiB Gateway *receipt-only* log). These drafts must not be merged as the production 28 KiB owner.  
**User decision:** Discard legacy HistoryStore records after an explicitly gated migration; reuse all seven 4 KiB pages on **each device independently** as extra data retention, *not* as a dedicated receipt store. No A/B mirror.

## 1. Physical Flash ownership (never imply contiguous capacity)

Each RAK463x separately has:
- Product: `[0x0C5000,0x0E5000)`, 128 KiB, existing role-owned store:
  Tracker = ObservationStore; Gateway = ProductCustodyStore.
- Geofence, Security, Config and relocated InternalFS/BLE: `[0x0E5000,0x0ED000)`, **unchanged and not spare**.
- Legacy History: `[0x0ED000,0x0F4000)`, 28 KiB, 7 pages — candidate
  supplemental store in the new role-aware KQ05 image.
- Bootloader starts at `0x0F4000`, unchanged.

The goal is **128 KiB + 28 KiB = 156 KiB of distinct physical
storage regions per Tracker or Gateway**, NOT one contiguous 156 KiB
partition and NOT a promise of 156 KiB usable payload. Format overhead,
reserve/recovery and torn writes reduce usable record capacity.

| Device storage role | Existing 128 KiB | Supplemental 28 KiB | Authority |
| --- | --- | --- | --- |
| Tracker | OBS1 observation records | Additional durable observation backlog/overflow | Only locally authentic ACK and durable release marker may retire a record |
| Gateway | OCQ2 exact durable custody objects | Additional exact durable custody objects/backlog | Only independently verified downstream *durable accept* may retire custody |
| Unassigned/conflicting | Do not mount | Do not mount | Fail closed |

Do not mount Tracker's supplemental log on a Gateway or vice versa, even
if the same PCB can switch software roles. A role transition requires
explicit, independently verified zero-liability and operator-visible
migration. No write overlap with legacy HistoryStore. The existing
HistoryStore `begin()/poll()` and any writer MUST be disabled in the
new role before opening the supplemental owner.

## 2. ACK contracts: distinguish TWO kinds of assurance

### Gateway BATCH_CUSTODY_ACK (new, proposed)

A Gateway may construct a signed **selective** ACK for a bounded set
(e.g. maximum 8) of *exact Tracker identities* for which it independently
holds a successfully committed/read-back protected object in either
its 128 KiB or 28 KiB store. It MUST use already provisioned,
per-Gateway/per-Tracker custody credentials and scope/grant; never root
keys or public test keys.

Binding must include the Gateway ID, Tracker ID, incarnation, key epoch,
each exact sequence/object digest, and an unambiguous ACK format/version
domain. The Tracker separately verifies the grant/key and matches
**each exact retained object**, then writes durable local release markers
idempotently. A syntactically valid packet, RX event, local RAM queue,
TX_DONE or CRC cannot authorize release. A failed/partial marker must
not result in a false release. Duplicates are harmless; overlapping
Gateway A/B proofs are idempotent.

It means: *Gateway accepted durable custody*. It **does not mean**
Backend has received the observation.

This is **not** the existing 56-byte per-record custody ACK and **not**
the current KQ05 Backend receipt protocol. It requires a new
authenticated selective Gateway-ACK contract and tests. Never reuse
wire type/crypto domain without explicit versioned proof.

### Backend-DURABLE receipt (existing KQ05 host proof; optional separate evidence)

A Backend-durable receipt means Backend stored exact observations; only
the Backend has authority to mint that proof. An intermediary Gateway
cannot mint a Backend receipt. It may be retained as a different,
explicitly typed status/proof in downstream reconciliation. **Never
confuse Gateway custody with Backend completion**, and never silently
turn a Gateway custody receipt into a Backend receipt.

**Release policy change:** For routine PERIODIC/History, once a valid
Gateway BATCH_CUSTODY_ACK has been durably applied on Tracker, the
liability moves to Gateway, as in the prior per-record custody protocol.
Gateway cannot reclaim that object merely because it has transmitted an
ACK; it must preserve until **authenticated Edge/Backend durable handoff**.
Critical EVENT remains on the proven per-event custody path pending a
separate decision/test.

## 3. Offline and multi-Gateway semantics

- No internet, no Backend, but authorized Gateway with free capacity:
  Tracker persists + transmits new-first, Gateway durably stores exact
  bytes (in existing or supplemental log) and signs selective custody
  ACK when appropriate. No Backend authorization lookup or cloud roundtrip
  required at ACK time; local grant must already be valid/provisioned.
- No Gateway reachable: Tracker retains observations in primary plus
  supplemental log. Newest location is preferred on RF, history drains
  opportunistically, without displacing retained unconfirmed records.
- Multiple Gateways: each ACKs only the **exact objects durably present
  locally**. Gateway A/B overlap is okay and deduplicated by Tracker
  identity + incarnation + sequence + protected-object digest. Gateway
  stores are independent; they do not require peer-to-peer communication.
  Edge/Backend deduplicate uploads using the same logical identity.
- On restored Edge/Backend connection, a Gateway transfers exact held
  objects and only reclaims once the next owner has durably accepted
  and authenticated a byte-exact handoff. Edge retains until Backend
  durable accept; no automatic TTL/oldest-first eviction of liabilities.
- Never delay current LIVE transmissions waiting for enough history to
  form a full ACK batch. Bound ACK batching by age, radio windows and
  regional duty-cycle; critical EVENT policy remains higher priority.

## 4. Independent page reclaim (no A/B mirroring)

- Two physically distinct log families with *one exclusive owner per
  device role*; each log must support Flash NOR 1->0 write constraints,
  commit-last/readback, boot scan, generation/role/identity binding,
  torn-slot recovery, duplicate detection and refusal on full.
- Reclaim only a complete 4 KiB page whose **every committed record**
  has an independently verified terminal downstream acceptance. A page
  with even one unresolved custody/observation must remain.
- No blindly timed erase, no erase solely because ACK TX succeeded, no
  erase of still-custodied data during migration. A/B full-page mirroring
  is not part of the design, but this does **not** waive crash-safe
  markers and recovery.
- If no reclaimable page exists, backpressure/fail closed and report
  capacity. No overwriting oldest outstanding data.
- Flash write/erase must use **one shared physical arbitration and
  completion owner**. History's old 28 KiB flash backend covers all 7
  pages and cannot be exposed to two independent writers. Limit the
  new supplemental owner by role, including SoftDevice async,
  quarantine/timeouts and proof of no overlap with nearby partitions.

## 5. Migration and acceptance gates

1. Introduce an exclusive role-aware supplemental log abstraction for
   28 KiB (not a naive 128+28 page-count increase). PC fake-NOR layout
   and format tests; prove boundaries and one-writer-per-region.
2. Add Tracker observation spillover/selection and Gateway custody
   spillover/handoff with all pending/restart/fault cases on PC.
3. Define versioned Gateway BATCH_CUSTODY_ACK with authenticated
   exact-object selective IDs, host crypto vectors, multi-Gateway
   duplicate/tamper, replay and key/epoch tests. Do NOT represent a
   Backend receipt as this ACK.
4. PC integrated disconnected test: ≥2 Gateways, 1+ Tracker,
   delayed/duplicate/out-of-order packets, one Gateway reboot/loss,
   no Backend during custody transfer, later Edge/Backend recovery;
   no lost/false-released record, with newer LIVE never stalled.
5. Prove no data loss before accepting initial legacy cleanout.
   Previous owner explicitly authorizes removing obsolete legacy
   History records, **but not ongoing custody, credentials, config,
   geofences, or any unknown/foreign Flash evidence**. Device-role-
   gated, operator-visible transition only; no automatic erase on boot.
6. Only after clean full PC regressions, separately check physical
   Flash arbiter, power cuts, concurrent BLE/LoRa and legal RF timing
   on 2+ RAK Gateways + Tracker. Do not flash devices in this ADR PR.

**Release verdict:** No product merge, no physical erase/upload, no
automatic data migration, and no field readiness claim from this ADR.
