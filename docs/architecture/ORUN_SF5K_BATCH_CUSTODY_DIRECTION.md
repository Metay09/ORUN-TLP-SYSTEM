# SF5K0 — PRODUCT_SECURE bounded batch custody: design candidate

Status: **PROPOSED DESIGN FOR INDEPENDENT PROTOCOL/SECURITY REVIEW — NOT A WIRE FREEZE, OWNER APPROVAL OF THESE NEW BYTES, OR RUNTIME AUTHORIZATION.**

Baseline: `main@20d71f6dbfa64c281ea181492d6ad5aa1ba20350`.
Cross-reference: draft SF5J2B1 PR #133, held pending this design.

## 1. Recover what ORUN already agreed and implemented

The previously reviewed **SF2/M4P4 HISTORY_SECURE** receipt supports **1..6 explicit History identities in one authenticated BACKEND_DURABLE receipt**, not a cumulative `ACK <= N`. The frozen codec is in `tlp_v2_history_secure.{h,cpp}`. SF3 `HistoryDeliveryCoordinator` already preflights an authenticated explicit-identity receipt, buffers out-of-order facts in a bounded RAM selective set, and only advances a contiguous *actual-record* prefix. Do **not** reproduce or silently change that codec/algorithm.

But this prior SF2 receipt is a **backend-authority proof for History**, not a Gateway-custody receipt for new opaque PRODUCT_SECURE objects. Its authority, frame family, logical identity namespace, data shapes and replay semantics differ. A Gateway may **never** mint `BACKEND_DURABLE`.

SF5B PRODUCT_SECURE §12 currently describes **one exact object per 56-byte `GATEWAY_CUSTODY_ACK` (type 0x05)**. It explicitly defers a new aggregate custody release protocol. Therefore a new aggregate *Gateway* ACK requires separate protocol, crypto, store and physical review. Do **not** reinterpret the frozen 0x05 fields or treat the History batch as a valid PRODUCT_SECURE custody ACK.

**Preserved user-level product intent:** offline low-rate sampling/reporting with at most one extra half-report-interval retry for contact discovery; trusted contact triggers bounded **multi-object transfer and batch ACK**, not a permanent stop-and-wait per-record cycle. Latest ACK is **not** authority to discard earlier records without explicit per-object durable proof.

## 2. Sender / Gateway / tracker responsibility

State-machine direction, not an activated implementation:

```
TRACKER: retain unacknowledged logical PERIODIC/EVENT records in ObservationStore
    |
    +-- offline: one existing exact protected probe frame; first extra probe
    |            at half the configured report interval; then radio-quiet
    |            except for a newly committed report/contact opportunity
    |
    +-- authenticated custody-capable contact:
           choose oldest retained not-yet-durably-released records
           stage up to 4 exact protected frames (1:1 with logical records)
           reuse cached exact bytes; new seals only with durable SecurityStore
           nonce, and before first TX persist/readback each frame in SF5C
           send bounded window (airtime, half-duplex, LIVE/EVENT priority)
                    |
         GATEWAY receives each exact opaque inner frame
           honest ACK-eligibility for EACH frame:
            * authenticated exact-object EDGE_DURABLE_ACCEPT, OR
            * own durable CustodyStore commit + verified readback
           no ACK for mere RX/TX_DONE/socket/BLE/RAM receipt
                    |
         GATEWAY: a SINGLE authenticated aggregate custody ACK listing ONLY
                  those independently durable-accepted exact objects
                    |
         TRACKER: authenticate source/credential+custody-policy tuple;
                  match EACH explicit digest to ONE retained/cache object;
                  no range inference; release ONLY proved members via
                  existing ObservationStore owner, with crash-safe progression
                  repeat/partial ACK safe; then refill window from oldest
           while pending > 0, continue gradual drain at an RF-safe rate
           on contact loss, fall back to sparse contact probes
```

The window bound `W <= 4` reuses SF5C `kMaxExactCustodyObjects == 4`: it does **not** limit the number of logical reports stored over hours or days. If 6 records accrued, transfer at most 4, receive a receipt naming the durable subset, then refill freed slots for the remaining 2. **The target is 2 aggregate ACKs for 6 successfully accepted records under a 4+2 grouping**, not 6 mandatory single-object ACKs. Lost/partial ACKs can need additional receipt transmissions.

The receive ACK is not permission to transmit all queued records without regional duty-cycle, collision, RX-window and battery budgeting. A candidate 15-second spacing inherited from draft SF5J2B1 is **not an approved fixed global policy**; replace it with measured, bounded airtime scheduling.

## 3. Candidate new batch ACK type; original 0x05 stays untouched

This section is **a candidate for review**, not an allocated type until registry review. Suggested new distinct `type 0x06` (`GATEWAY_CUSTODY_BATCH_ACK`), with direct and reviewed one-hop-relay paths required before product cutover. An initial codec may be tested direct-only on the host, but production must NOT advertise relayed batch custody until the reverse ACK route is implemented and qualified. All multi-byte fields use network byte order.

```text
offset  bytes  field
0       1      version = 0x02
1       1      type = 0x06 (NEW / CANDIDATE, subject to registry review)
2       1      flags (bit 0 RELAY_ALLOWED; all other bits zero)
3       1      count in [1, 4]
4       8      tracker_device_id
12      8      gateway_device_id
20      4      key_epoch
24      4      custody_policy_floor
28      4      custody_grant_generation
32      9*N    entries: object_len_u8 || object_ref_be64
32+9*N  16     truncated HMAC-SHA256 tag
```

Candidate total length = `48 + 9 * N` bytes; **57, 66, 75 or 84 bytes** for 1..4 entries. Sender uses the *exact* inner 84/100-byte PRODUCT_SECURE frame, not an optional relay wrapper. A future RESULT family requires separately reviewed support.

Canonical candidate:
- entries strictly ordered by `(object_len, object_ref)`, no duplicates;
- `object_len` must be a supported exact frame length, 84 or 100 B;
- each `object_ref` = first 8 bytes of `SHA256(exact_inner_frame)`;
- reject duplicate, unknown, ambiguous or multiple-match refs; never treat an 8-byte lookup hint as the authenticator;
- all entries belong to the same tracker credential lifetime, policy floor, enrolled custody gateway and exact grant generation; no cross-gateway/floor/epoch pooling in one batch.

### MAC / authority direction (NOT yet implemented)

Preserve custody authority isolation from COMMAND and BACKEND_DURABLE, and preserve the SF5B per-tracker/per-Gateway enrollment tuple. Prefer a **separately domain-separated 32-byte HKDF batch ACK key** from the existing root PRK and authenticated custody tuple, with a dedicated label (candidate: `ORUN-TLP-V2-GW-CUSTODY-BATCH-ACK-v1`), not a fleet key or a tracker-root export to Gateway.

Candidate MAC input:
```text
ASCII("ORUN-TLP-V2-CUSTODY-BATCH-ACK-MAC-v1")
|| raw_ack_bytes[0 .. 32 + 9*N - 1]
|| for each listed entry, in wire order:
      full_SHA256(exact_inner_frame_bytes)[32]
```

Tag is first 16 bytes of standard `HMAC-SHA256(K_custody_batch_ack, MAC_input)`. Full digest binding is essential: the transmitted 64-bit ref alone is insufficient. Before code freeze, independent KDF/HMAC host golden vectors, negative vectors, RAK CC310/HKDF/HMAC coexistence proof, packet-size/RF airtime feasibility, and security audit are mandatory. Backend/security authority may delegate only this per-enrolled-Gateway custody capability to a Gateway, never `K_root` or command authority.

Tracker must calculate candidate digests from **immutably read-back cached exact frames** and use the active credential and durably installed custody policy. The existing 0x05 verifier does NOT implicitly accept 0x06. The current `SecurityStore-v3` persistent custody authority state is still UNIMPLEMENTED; until present, ALL production 0x06 acceptance must be disabled.

### No implicit cumulative ACK

No numeric watermark, `delivered_through=N`, newest record success, link RSSI, RF reception, network heartbeat or unverified Gateway claim can release another object. ACK count N proves ONLY its explicit N verified digest bindings. A valid partial ACK of records #1 and #3 leaves #2/#4 pending and eligible for selective retry. One ACK may represent Gateway durable storage or authenticated Edge durable-accept, independently per entry, but **not** raw transport success.

## 4. Crash, durability, replay, loss and flash wear

Before applying *any* member: decode canonical length/count/order, verify enrolled Gateway state/floor/generation/key-epoch/tracker ID, find a unique cached exact object for every member, compute ALL full digests and verify the ONE batch HMAC. **Preflight entire batch** and reject on any ambiguity/torn flash/read failure; no partial release based on an unauthenticated subset.

After cryptographic preflight, durable releases/clear operations must use the existing sole `ProductRecordStore` / `ObservationStore` flash owner and its asynchronous completion semantics. `ObservationStore::requestRelease/takeReleaseResult` and `requestClearExactObject/takeControlWriteResult` are currently separate operations. The new batch release coordinator therefore requires an explicit crash-consistent, idempotent state machine, including:
- sequence/order and ownership of release-vs-exact-cache-clear;
- physical power-cut at every mutation boundary and late async completion;
- no space freed prematurely while an ACK is not fully authorized;
- ability to reapply one complete authenticated batch after reset without double-loss;
- already durable accepted members excluded safely from future retry; unaccepted members still available;
- partial ACK, duplicate ACK, delayed/out-of-order ACK and missed RX rendezvous;
- selective retry of members absent from an authenticated batch; never all-or-nothing resend unless that is the only safe fail-closed recovery;
- no erasure/rotation solely to checkpoint every received ACK; quantify control-journal wear and data-page release effects;
- no new flash partition and no unreviewed schema migration.

Existing SF3 `HistoryDeliveryCoordinator` is the **algorithmic model** for bounded selective acknowledgement and exact-present-record prefix advancement, but not a source of authority or a drop-in replacement for PRODUCT_SECURE flash release.

## 5. Retry / power policy already agreed

- For a configured report interval T, permit one extra contact retry **approximately T/2** after the first failed-contact attempt, subject to an RX rendezvous/power budget. Then await a real new report-period contact opportunity; avoid free-running short retries.
- Contact may be established by **one authenticated and exact-object-matched custody response**, or a separately reviewed authenticated custody-capable session signal; an ACK for record #6 does not retroactively acknowledge #1..#5.
- Once trusted contact exists, transition to a bounded window of up to 4 objects. Refill only after verified durable releases and explicit capacity availability; respect urgent EVENT and current report priority, GNSS/sensor power ownership, radio half-duplex, duty cycle and fairness against other trackers.
- On timeout/absence, stop rapid drain; preserve the exact objects and reduce to sparse report-aligned probing.
- If new logical records accumulate during drain, the **proven confirmed-custody service rate** must exceed the normal production rate with margin for the supported deployment topology; measure rather than assume.
- Purely logical records may exceed the four cached slots: postpone sealing until a slot is safely free, instead of evicting or encrypting every old record.

## 6. Compatibility and implementation sequence

**Do not cut over production** until all of these are reviewed and qualified:

1. SF5K0: independent approval or amendment of this aggregate custody *candidate* (including wire type allocation, MAC/domain, exact object limits, selective ordering, rollback and out-of-order receipt).
2. SF5K1: host-only 0x06 codec and independent KDF/HMAC vectors, malformed cases, negative authorization, length/order and collision tests. **Never use HISTORY_SECURE BACKEND_DURABLE as Gateway custody authority.**
3. SF5K2: minimal Gateway exact custody batch receipt owner; each member durable via local flash or verified Edge proof before included; duplicate idempotency; no soft ACK.
4. SF5K3: tracker verified receipt admission + asynchronous durable selective release; power-cut/late callback tests; encryption-cache bound 4. Preflight and idempotency, then refilling.
5. SF5K4: replace PR #133's **single-object `onCustodyReleaseCommitted` driven serial stop-and-wait policy** with a bounded multi-object contact scheduler. Reuse its dormant store-before-send, exact cached bytes, report-aligned offline retry and security gate; **do not merge its current one-by-one drain as the intended production sender**.
6. Host suite + PlatformIO compile, physical dual-device RAK4631/SoftDevice/current/powercut/LoRa trials, Gateway/Edge durability, and finally explicit TLP V1 retirement authorization.

These features are NOT included in this design PR. SF3 HISTORY_SECURE, TLP V1 POSITION, SF5B PRODUCT_SECURE 0x04, existing 0x05 single-object ACK and all prior wire golden fixtures must remain byte-for-byte unchanged.

## 7. Design acceptance matrix (to implement as deterministic tests)

| Scenario | Required result |
|---|---|
| 6 logical records, 4 exact slots | window 4 + next window 2, not 6 encrypted objects |
| all four durable at Gateway | one valid 4-entry ACK permits selective durable release of those four |
| first 2 durable, last 2 uncertain | ACK lists 2 only; 2 pending exact objects remain |
| 1 and 3 durable, 2 and 4 lost | selective ACK {1,3}; retry only 2 and 4 |
| ACK for newest #6 alone | older #1..5 remain retained |
| single ACK lost after Gateway committed | exact retransmission/duplicate idempotency; no new counter |
| 8-byte ref collision / duplicate entries | fail-closed; no local release |
| forged MAC / unknown Gateway / revoked grant / epoch mismatch | fail-closed; zero release |
| read failure or torn object cache | fail-closed; do not infer ACK from record ID |
| crash after first durable release of a 4-entry batch | recover conservatively; retry/replay safely without false ACK |
| gateway flash full or Edge proof absent | never include uncertain object in aggregate ACK |
| two enrolled Gateways | accept each only under its own exact current custody authority; no cross-grant key |
| mobile Gateway / no Internet | locally durable custody suffices; backend delivery may happen later |
| EVENT concurrent with backlog | bounded priority without starvation of long-term drain |
| 1h and 2h offline report cadence | one half-T extra probe, then only report-triggered probes |
| TLP V1 and frozen SF2 History bytes | unchanged; no family/type reinterpretation |

## 8. Explicit hold / reviewer questions

- Approve/alter proposed type `0x06` and its unmodified one-hop relay wrapping in BOTH directions; this requires a new explicit allow-list addition, type registry/router tests and physical reverse-hop qualification. Merely supporting relayed uplink is not sufficient.
- Confirm a **four-member ACK** is the initial PRODUCT_SECURE batch ceiling (aligned to the *existing* exact ciphertext cache), distinct from the older six-ID HISTORY receipt. Do not pretend two authorities use the same wire semantics.
- Decide canonical ordering `(object_len, object_ref)` and 9-byte entry encoding after collision/airtime review.
- Review separate HKDF key versus reuse of current per-Gateway custody ACK key; this document proposes separate key to avoid cross-protocol confusion.
- Define memory budget, RX-open/rendezvous, ACK aggregation timeout, burst pacing, and retransmission after partial ACK from measured LoRa and product energy evidence.
- Before writing security code, close `SecurityStore-v3` independent persistent custody grants and the SF5J2B1 recovered-cache credential-lifetime proof, without replacing an unauthenticated header comparison for real cryptographic binding.

**Document-only review candidate. It makes no flash/radio/ACK/runtime change and claims no passing tests, new codec, or actual aggregate custody capability.**

## 9. 2026-10-16 one-hop relay / full RF-path engineering audit (blocking)

**Scope:** The calculations below are illustrative **SF11/BW125/CR4/5, 869.525 MHz** using the present reference radio config. These values are independently reproducible from the standard LoRa symbol airtime equation and match the existing SF5B 100B=2.215936s and 116B=2.461696s reference numbers. A **4-entry** batch ACK is a PROPOSAL, not running code. No deployment duty-cycle, radio reliability or battery consumption has been physically measured by this change.

### 9.1 Critical source-code finding: relay path is not actually present for PRODUCT_SECURE

Existing \`firmware/src/network_service.cpp\` forwards only legacy TLP v1 \`POSITION\` with a 15-byte wrapper and one-hop \`hop_count=1\`. Its \`relay_config.h\` delay \`kMinimumDelayMs=1200\`, \`kMaximumDelayMs=4200\`, queue capacity **four**, relay dedupe **16**, Base dedupe **32** are **V1 runtime facts**.

SF5B \`V2_RELAY_FORWARD\` proposes a DIFFERENT 16-byte opaque one-hop wrapper for v2 types 0x01/0x04/0x05, with unchanged encrypted inner bytes and no nested wrappers. \`RadioManager::handleReceivedPacket()\` currently queues plain 0x04 PRODUCT_SECURE into a **single pending receive mailbox**; it does NOT route 0x02 V2_RELAY_FORWARD, nor 0x05 ACK, nor hypothetical 0x06 batch ACK. The generic RX callback event queue has capacity four, **not** a verified four-frame product window; the live ProductSecure mailbox admits one pending object only.

Therefore NO product feature may assume that current relays can transport batch data OR return a batch receipt. Implementation must extend v2 route + uplink/downlink dispatch + signed batch ACK acceptance, using the existing radio/driver gate, without changing frozen legacy V1/SF2 bytes. The hypothetical 0x06 must be explicitly added to the reviewed v2 relay allow-list before enabling this route.

**Gateway signs the batch ACK** under separately enrolled custody authority after individual objects become durable. A relay NEVER claims or creates Gateway durable custody; it only forwards an unchanged authenticated ACK through its own outer wrapper. The reverse link (Gateway→Relay→Tracker) is independent of whether uplink used a relay. A Gateway RX-only link or a relay unable to hear the Gateway's ACK is **not** a usable bidirectional custody path.

### 9.2 Every physical transmission for one group

For a one-hop return route, the RF path is:

\`\`\`text
Tracker -- 100B protected PERIODIC --> Relay
Relay -- 116B opaque wrapper --> Gateway
Gateway -- N-entry batch ACK (48+9N B) --> Relay
Relay -- 16B wrapper + unchanged batch ACK --> Tracker
\`\`\`

Only Gateway/Edge durable acceptance admits ACK membership. \`TX_DONE\` at Tracker or Relay is never custody. All LoRa TXs are half-duplex: a forwarding relay CANNOT hear the next tracker packet while it is transmitting a previous packet; Gateway also cannot hear uplink while sending an ACK.

LoRa symbol time uses \`2^SF/BW\`; airtime includes preamble, explicit header, CRC, CR4/5 and low-data-rate optimization. Results:

| Number of 100B PERIODIC records | Candidate ACK B | ACK wrapper B | Direct total airtime (s) | One-hop uplink + one-hop ACK return total airtime (s) |
|---:|---:|---:|---:|---:|
| 1 | 57 | 73 | 3.613 | 7.799 |
| 2 | 66 | 82 | 5.992 | 12.804 |
| 3 | 75 | 91 | 8.372 | 17.809 |
| 4 | 84 | 100 | 10.752 | 22.815 |

These totals are **SUMS of transmissions by different nodes** on one RF collision domain. They are NOT wall-clock completion estimates, nor duty-cycle on one transmitter. Source TX, Relay TX and Gateway TX each need their own independent duty/energy budget. A relay that happens to hear the Gateway directly cannot shorten the reverse route by an unproven link assumption.

### 9.3 Why a 4-frame back-to-back burst is broken with an intermediate relay

For the CURRENT V1 forwarding delay range only, a relayed 100B uplink entails up to \`4.2s defer + 2.462s relay TX = 6.662s\` after the tracker finishes its own 2.216s TX. If the tracker sends the next 100B frame immediately (~2.216s later), that frame starts while Relay may be forwarding the first. A half-duplex radio cannot capture both; missing one frame creates false assumptions about the group's end.

With one selected single-threaded relay, assuming its V2 scheduling adopted **exactly** the current V1 delay bounds and with NO channel contention, a conservative source packet **start-to-start** lower bound is:

\`\`\`text
2.215936s (source packet TX)
+ 4.2s (worst allowed forward scheduling defer)
+ 2.461696s (relay packet TX)
+ switching/processing/clock guard
= 8.877632s + guard
\`\`\`

This is a **necessary isolated-link serialization lower bound**, NOT a production fixed delay or a proof of availability. Actual V2 Relay queue, scheduling bound, radio arbitration and collisions must be measured before choosing slot spacing. A relay receiving only some of the packets, duplicate forwards from multiple relays, or a separate Gateway downlink can extend this bound substantially. A group-end rule based only on silence fails when a packet, group header, or the final packet is lost or forwarded late.

### 9.4 10-second tracker RX rendezvous can miss even a successful ACK

\`radio_config::kWindowedRxAfterTxMs = 10000\` is a **development-default 10-second RX window**, opened on a terminal tracker TX event in \`radio_manager.cpp\`; it is NOT already an authenticated batch-return rendezvous.

With one-hop uplink + one-hop return, 4-entry batch ACK 84B and its 100B reverse wrapper, the return time measured **from completion of the LAST tracker 100B TX** is:

\`\`\`text
uplink relay scheduling delay    1.2 ... 4.2s (V1 bounds, hypothetical V2)
uplink relay wrapper TX           2.461696s
Gateway durable accept/queue      G seconds (UNKNOWN; 0 only in lower-bound)
Gateway 4-member ACK TX           1.888256s
downlink relay scheduling delay  1.2 ... 4.2s (NOT IMPLEMENTED; hypothetical)
downlink relay wrapper TX         2.215936s
additional radio/CPU contention   S seconds (UNKNOWN)
-------------------------------------------------------------
elapsed before tracker gets ACK   8.965888 ... 14.965888s + G + S
\`\`\`

Thus **10s is insufficient at the worst of even these idealized legacy relay bounds**. A one-hop-only uplink and DIRECT ACK still requires the source to hear Gateway directly, which is NOT guaranteed. For all four source transmissions with worst-case isolated relay serialization, last relayed packet can reach Gateway at least about **35.51 seconds** after the first source TX starts, and an ideal return via Relay would finish around **43.81 seconds** from the start (all G/S/guard zero). These are best-case schedule calculations with assumed bounds, NOT maximum real elapsed time.

Do NOT simply expand every tracker RX window to 20, 30 or 60s: this can destroy livestock battery life, and still does not bound a busy Gateway. A future control owner must provide a **bounded ACK rendezvous / short scheduled RX opportunity** (or a justified per-session extended RX commitment) that works for direct AND selected relay paths, with explicit sleep wakeup, local clock tolerance and lost-ACK recovery. ACK transmission after Tracker's window ended is not an ACK; it is an expensive missed downlink.

### 9.5 Batch boundary / start / end — silence is not a protocol

The design MUST explicitly define how sender and Gateway agree on:
- same tracker credential lifetime and bounded **transfer-session ID**, current group window, count in [1,4], and membership;
- expected number of slots / latest allowed Gateway receive deadline, including relay timing and missing frames;
- a deterministic **ACK-return rendezvous** after the Gateway's last allowable receipt/commit deadline;
- ability to ACK fewer than expected frames (or no frame), without treating a missing final frame as "the group ended";
- receiving 2 frames when only 2 exist, and receiving 2 of 4 frames after two were lost;
- a session retry that reuses identical cached ciphertext, never counter-burning to satisfy a timeout;
- delayed/duplicated group control and ACKs arriving after the sender chose another group or Gateway.

Candidate methods to evaluate **before choosing** (none frozen): short authenticated window descriptor/END control; authenticated per-window scheduling without modifying existing 0x04 bytes; or a finite fixed-slot batch protocol with a separate authenticated session grant. All add some downlink/airtime/RAM/security complexity. Timer silence alone with an arbitrary threshold is NOT an acceptable final batch end or custody release rule. A group's ACK membership remains per-object exact digest, independent of untrusted framing/scheduling hints.

### 9.6 Fleet, Gateway and Relay shared-airtime constraints

A **single** tracker backlog of 20x100B PERIODIC, in ideal groups of 4:
- Tracker TX = \`20*2.215936 = 44.319s\`;
- chosen Relay **uplink** TX = \`20*2.461696 = 49.234s\`;
- Gateway **downlink** TX = \`5*1.888256 = 9.441s\`;
- chosen Relay **ACK downlink** TX = \`5*2.215936 = 11.080s\`;
- aggregate shared-channel raw airtime = **114.074s**, not 44.319s;
- tracker, relay and Gateway RF receive/listen times add POWER cost but do not add channel TX airtime.

If **each** of M livestock devices has 20 backlog records and all traverse the SAME relay, with no simultaneous independent channels:

| M trackers | Combined one-domain TX airtime (s) | Channel load if all drained within 1h | One shared relay's TX alone (s) |
|---:|---:|---:|---:|
| 1 | 114.1 | 3.2% | 60.3 |
| 10 | 1,140.7 | 31.7% | 603.1 |
| 30 | 3,422.2 | 95.1% | 1,809.4 |
| 50 | 5,703.7 | 158.4% | 3,015.7 |
| 100 | 11,407.4 | 316.9% | 6,031.4 |

100% raw occupancy is impossible before collisions; **31.7% is also not a safe contention target** for random unslotted LoRa. The existing 869.525MHz radio reference band falls within the Turkish BTK 869.4–869.65MHz row: **<=10% duty cycle OR qualifying LBT+AFA**, with limits/conditions to assess against the actual equipment (BTK official technical measures, table 1 item 21). If the 10% duty-cycle path is used, **360s/hr per transmitter** is an optimistic planning boundary. The example **single relay carrying 10 trackers x 20 reports** already requires ~603s of relay TX alone, so it cannot fit in one hour under that path; 50 trackers require ~3,016s of relay TX plus other traffic. A single Gateway with 50 trackers x 20 reports needs ~472s of ACK downlink TX alone, also above 360s/hr. This is a topology bottleneck even if every TRACKER individually has spare airtime.

These numbers are upper stress examples and ideal no-loss transmission calculations. A deployed site can use multiple Relays/Gateways and/or separated RF channels/domains, but no 50-node all-via-one-relay one-hour drain guarantee may be promised.

Do **not** claim a permissive PHY maximum ("255B packet", or "20 reports only 44s") proves multi-device product capacity. Regulatory, collision-domain and gateway/relay per-transmitter duty must be enforced/qualified before production. A heavyweight adaptive airtime optimizer is optional; **an approved compliance-safe minimal TX gate is not optional**.

### 9.7 Variables that must enter a verified scheduler

| Family | Required measured/configured inputs | If missing |
|---|---|---|
| RF PHY | frequency, SF7..12, BW, coding rate, power/ERP/antenna, preamble, CRC, exact frame length, TX timeout | keep conservative reviewed SF11; do not infer a faster link |
| Link/topology | direct/one selected relay, route to send ACK back, asymmetric RSSI/SNR and link budgets, channel/error statistics | no claimed ACK-capable custodian |
| Half-duplex | Source TX, relay RX+TX, Gateway RX+TX, relay reverse TX and radio turn-around/driver gate | no consecutive unscheduled burst |
| Scheduling | relay bounded defer, queue depth, competing forwarded packets, lost final packet, jitter/clock error, inter-node fairness | no arbitrary silence-end timer |
| Custody | Gateway durable queue capacity and erase/commit/readback latency; valid Edge durable accept/fallback; re-ACK duplicate | no custody ACK on merely received frames |
| Security | separately enrolled Gateway custody grants, key_epoch/credential lifetime, exact-inner-frame full digest HMAC, replay/revocation, out-of-order | no release; fail closed |
| Power | Tracker TX/RX/MCU/crypto/flash power & RX wake/sleep duty, Relay potentially continuous RX | no battery-life claim |
| Multi-device | 1/10/30/50/100 nodes, multiple Relays/Gateways, hidden nodes, duplicate forwards, simultaneous emergencies | no single-track airtime extrapolation |
| Compliance | region and date, duty-cycle/LBT+AFA choice, per-transmitter accounting, channel access; reboot-safe restriction | no product RF enablement without a safe limit |
| Recovery | partial batch, ACK lost, source reset, relay reset, Gateway reset/flash full, delayed ACK, key rotation, flash tearing | exact bytes retained; no silent data deletion |

The 4-object encrypted cache does not itself provide a 4-message event queue or four independent RX buffers. Owner scheduling must drain \`RadioManager\`'s mailbox and host queues before they overflow. Forwarders with \`relayForwardingEnabled()\` may require CONTINUOUS radio listening under the existing \`RadioListenPolicy\` resolver; **a livestock tracker that also relays traffic has a different battery budget** from an ordinary windowed tracker.

No fixed interval between source frames, total ACK wait, airtime budget, channel count, exact PHY setting or ACK-grace timer is approved by this annex. Selection follows bounded simulation, host tests and physical measurement, not guessed constants.

### 9.8 Required scenarios / stop conditions before SF5K1+ physical deployment

- Direct, tracker→Relay→Gateway only, Gateway→Relay→Tracker only where feasible, both hops via same relay, and asymmetric/no-return topology.
- Exactly 1, 2, 3 and 4 queued logical packets; selective 1/3 accepted of 4; 5/6/20/100 retained overall (4 encrypted slots only).
- Relays 0/1/2 in radio range competing to forward same original; only **one hop per packet** and no nested relay, but multiple candidate relays may produce duplicates unless scheduled/deduped.
- Head-of-line blocking: first or final frame missing; Gateway sees incomplete group; ACK route delayed beyond normal RX; group timer must not make future ACK undeliverable.
- Priority EVENT/COMMAND preemption, new PERIODIC creation while backlog drains, Gateway flash busy/full and Edge unavailable.
- 10 / 30–50 / ~100 trackers, heterogeneous cadence, synchronised report bursts and random offsets; all-via-one-relay vs partitioned site.
- Verify minimal lawful TX admission and independent budgets at Tracker, selected Relay and Gateway, including reboot.
- Physical tester must log **per-hop TX start/end, RX start/end, packet lengths, queue occupancy, Gateway durable completion, ACK start/arrival, wake times, battery current and expected/actual receipt bitmap**, plus duplicate and failed paths.

**Final disposition for this audit:** SF5K0 batch ACK direction remains useful, but the previous simple "four consecutive TX followed by silence-driven ACK" implementation prescription is **REJECTED for production**. The blocker is absent v2 forward+reverse relay runtime and unproven half-duplex/rendezvous timing, NOT the cryptographic notion of one aggregate receipt. Do not merge #133's serial sender as a workaround. Treat two-way relay and explicit bounded session scheduling as mandatory production design review gates; preserve main and existing bytes.
