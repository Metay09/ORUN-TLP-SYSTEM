# ORUN product RF cutover — single integration gate (SF5K / MAC v0)

**Status: engineering implementation direction; NOT a released RF/wire/security implementation.**
**Baseline:** main at `20d71f6dbfa64c281ea181492d6ad5aa1ba20350`, reviewed with MAC v0 PR #137 and SF5K drafts #134–#140 on 2026-10-17.

## Scope and retained product rules

The first product RF chain is **TRACKER -> [one-hop transport-only RELAY] -> custody-authorized GATEWAY -> durable Edge/backend**. Mobile Gateway is also a Gateway *role* and must satisfy the same custody and authorization requirements; it is never implied by Relay. Product observation records remain durably stored on the Tracker until actual authenticated custody acceptance.

Retain the existing product `PRODUCT_SECURE` exact inner frame (84/100 B, authenticated AAD/path flags, `record_incarnation` + `record_sequence` encrypted), `ObservationStore` single durable mutation owner, Gateway/Edge durable-evidence requirement and the per-Gateway custody grant. Do not let TX_DONE, a mere RF receipt, an unauthenticated link heartbeat, or a numeric watermark release data.

**Initial ACK direction: the bounded explicit-identity/digest batch ACK in SF5K0 (up to four separate encrypted exact objects per window).** Only explicitly authenticated exact objects may be released. A partial authenticated ACK for #1/#3 does not imply #2/#4. Host-only candidate 0x06/HMAC tests remain non-production until a complete verified grant and runtime admission path exists.

Normal ACK may be requested less frequently than every record, and urgent EVENT should get the first RF-safe response opportunity. Scheduling frequency is a policy separate from the ACK's **authorization semantics**. Do not equate “ACK after four packets” with “four records implicitly accepted”, and do not force a six-hour wait when the four-slot exact-object cache needs recovery.

## Disposition of MAC v0 PR #137

**Keep as engineering candidates:** report-aligned wakeups; tracker-specific randomized report offsets; dedicated short post-TX ACK rendezvous measured on hardware; Relay half-duplex arbitration and bounded forwarding; channel assessment as a collision mitigation, not a regulatory shortcut; direct-Gateway path preference where safely implemented; rolling per-transmitter airtime/duty accounting; emergency priority; explicit online/offline and backlog backpressure. Start RF qualification at SF11 and measure other SFs without assuming universal coverage or capacity.

**Not authorized for product wire merge:** R11 `base + 32-bit bitmap` cumulative ACK and `ACK_REQUESTED` bit; its implicit range semantics conflict with exact-object custody proof in SF5B §12 and SF5K0 §3. The Gateway normally cannot read the encrypted tracker-wide `record_sequence`, so it cannot directly create that bitmap from today's opaque protected frames. Any alternative requires a separately reviewed, authenticated metadata/wire/key-epoch/grant/flash design, not an interpretation of old bytes.

**R3 is not a production-capable eight-object batch:** physical `kMaxExactCustodyObjects == 4`. A report wake may eventually send multiple sequential windows *only after prior verified releases and available cache slots*. There must not be eight unacknowledged sealed custody objects simultaneously. This is a distinct logical-record retention limit (the product region holds far more than four observations).

**R4 path switching must be redesigned:** `RELAY_ALLOWED` is authenticated AAD. An existing exact ciphertext cannot have this bit flipped after a missing ACK. Either a path is approved at the initial one-time seal or a separately security-reviewed new-object lifecycle is required. A direct-only sealed backlog that later needs relay must not be silently reprotected or stranded without a defined remedy. Test/direct-relay policy is a cutover gate.

**R1 timing is a measurement hypothesis:** SF11 `X=10s`, `w=0.5s` and bounded Gateway commit times come from a model, not SX1262/RX-mode/Edge-flash qualification. RX preamble detection must keep radio active through the full ACK. The Relay-to-Gateway uplink and Gateway-to-Relay-to-Tracker downlink need real traces before an ACK rendezvous is frozen.

## A discovered real-storage semantic and power-cut policy

Production `ObservationStore::findExactObject(id)` returns `kNone` as soon as the backing logical record is **durably released** (whether or not old control bytes remain physically on flash); `requestClearExactObject(id)` is then `kAlreadySatisfied`. This deliberately prevents a release callback from being followed by an imaginary second “physical clear” transaction.

Consequently, if a four-member authenticated ACK has released member 1 and the Tracker resets, the same four-member ACK **cannot be independently reauthenticated from the public exact-cache lookup**. It is unsafe to claim idempotent all-member replay. The safe first-release policy is:

1. Verify the entire original batch (full exact-object digests, authorized credential epoch/grant and HMAC) BEFORE releasing any member. Verify store-side presence/match for every member.
2. Durably release each **explicitly authenticated** member through the one `ObservationStore` owner. Read back its release marker before advancing. Exact-object cache ownership ends with that committed release.
3. If power is lost after a proper release, that member stays released. Other members remain retained with exact ciphertext. At a later authenticated contact, the Gateway creates a **NEW subset ACK naming only the still-retained exact objects**. Do not replay the old full batch as authority or infer other members from a released prefix.
4. Missing Gateway/reissued subset ACK means records remain on Tracker. Never clear those retained records simply to free the four cached slots. If the platform ever needs offline receipt recovery without a new ACK, that needs a separately designed durable grant/proof journal.

SF5K3A includes a **host-only adapter to the actual flash-backed `ObservationStore`**, with full/partial ACK release, reboot mid-batch, mismatched exact bytes and read-failure regression (`firmware/tests/sf5/test_sf5k3_observation_store_port.cpp`). The user ran `bash tools/ci/run_sf5k3a_host.sh` on 2026-10-17, confirming both initial tests PASS locally. This is **not** production ACK admission.

Next, `ProductRecordStore::custodyCandidateMatchesRetainedExact` adds a **strictly read-only product-owner preflight** (retained identity, not released/uncertain, exact cached bytes match, no pending flash owner mutation). It NEVER authenticates Gateway custody authority or HMAC, and it NEVER releases a record. The SF5F fake-flash suite tests pending/in-flight, mismatched bytes, read errors, async operation, reboot and released-record refusal, and the focused local script now runs this test via `--custody-preflight-only`. **This new extension is awaiting a real local compile/run result**.

A true production authenticated `ProductRecordStore` release adapter, durable Gateway custody grant and HMAC admission remain blocked, as do hardware/physical RF and power-cut validation.

## Execution order — finish the real product chain

1. **One exact-object direct round trip:** seal and read back a stored PERIODIC; Gateway owns real durable custody (or signed Edge acceptance); verify enrolled custody ACK and release readback on Tracker. Do not enable an unverified release path.
2. **One-hop reverse Relay path:** route exact 0x04 and 0x06 through the reviewed 0x02 wrapper in BOTH directions; Relay never mints or changes ACK; validate RF timestamps, half-duplex, ACK window and lost/late ACK behavior on actual devices.
3. **Partial 1/3 of 4 + crashes:** missing first/last frame; reboot after one committed release; reissue fresh authenticated subset ACK; 20+ logical records retained across cycles; ensure cache refills only after available slots.
4. **SF11 collision/airtime + power:** 1, 10, 30–50, ~100 per RF domain with feasible topologies; verify per-transmitter duty, CAD limitations, EVENT preemption, battery RX current and data retention during outage. Do not promise flat 30-via-one-Relay capacity without an actual link budget and capacity check.
5. **Mobile Gateway/Edge/backend cutover:** same custody authority rules offline, gateway storage exhaustion backpressure, signed durable Edge handoff, backend dedupe, app exposure; then explicit production V1 retirement.

**Release gate:** host sanitizer tests + integrated host tests (real ObservationStore), RAK4630 target build, physically logged Tracker/Relay/Gateway flash and RF, loss/reset fault injection, power and legal duty review. No claim of end-to-end production readiness before these pass. Keep `main` and live V1 behavior unchanged until explicit cutover.
