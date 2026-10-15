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

This section is **a candidate for review**, not an allocated type until registry review. Suggested new distinct `type 0x06` (`GATEWAY_CUSTODY_BATCH_ACK`), direct path first. All multi-byte fields use network byte order.

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

- Approve/alter proposed type `0x06`, direct-only receive initially, and whether a relay wrapper may carry its unmodified bytes; this requires new type registry and router tests.
- Confirm a **four-member ACK** is the initial PRODUCT_SECURE batch ceiling (aligned to the *existing* exact ciphertext cache), distinct from the older six-ID HISTORY receipt. Do not pretend two authorities use the same wire semantics.
- Decide canonical ordering `(object_len, object_ref)` and 9-byte entry encoding after collision/airtime review.
- Review separate HKDF key versus reuse of current per-Gateway custody ACK key; this document proposes separate key to avoid cross-protocol confusion.
- Define memory budget, RX-open/rendezvous, ACK aggregation timeout, burst pacing, and retransmission after partial ACK from measured LoRa and product energy evidence.
- Before writing security code, close `SecurityStore-v3` independent persistent custody grants and the SF5J2B1 recovered-cache credential-lifetime proof, without replacing an unauthenticated header comparison for real cryptographic binding.

**Document-only review candidate. It makes no flash/radio/ACK/runtime change and claims no passing tests, new codec, or actual aggregate custody capability.**
