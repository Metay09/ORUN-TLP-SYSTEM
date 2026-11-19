# KQ05 — Gateway scheduled 1..5 ACK over real Gateway loop boundary

**Status: DRAFT; PC verification NOT YET RUN.**
Parent: owner-confirmed PR #352 PC PASS at
`c834ecaaaa68f113e2ce13a7a9ca6f6d7e2f692b`
(Tracker 0x0A RadioManager mailbox -> real V3 HMAC -> independent OBS1
durable release markers; earlier OCQ2/LoRa transport regressions PASS).

## Explicit opt-in

`ORUN_KQ05_EXPERIMENTAL_GATEWAY_BATCH_RF`, combined with the previously
required `ORUN_KQ05_EXPERIMENTAL_ACKLESS_LIVE` and Tracker TX pilot
flag, selects the batch pathway. **No existing PlatformIO environment
defines the group RF flag.** Normal ORUN still uses its existing
single-object Gateway type 0x05 and Backend type 0x09 paths. This PR
does not upload/erase/repair any real device, nor activate live RF.

## Gateway loop composition

- `sf5l0_gateway::begin` still creates the physical identity-bound
  `Sf5l0BenchGrantSource` (signed OGK1 first, then RAM grant),
  `GatewayProductStartupGuard`, and actual `ProductCustodyStore`.
- In opt-in mode it instantiates `Kq05GatewayBatchCollector`,
  `GatewayProductLiveIngress` and `Kq05GatewayBatchRuntime`.
  **It does not instantiate or execute the old per-record
  `GatewayCustodyAuthorizedIngress`/type 0x05 TX owner in the same
  Gateway image.** The rest of Gateway Edge/BLE and previous
  role/boot/commissioning guards are kept.
- Each group contains at most **five previously confirmed-durable
  OCQ2 handles for one Tracker+key epoch**; the initial Flash commit
  must finish before eligibility for a HMAC ACK.
- **Exactly five**: auto request a group ACK immediately; **one to
  four**: start the partial timeout at the FIRST durable handle
  and never extend this timer when later items arrive. The provisional
  default is 120,000 ms (2 minutes), configurable for PC verification.
  This is a HOST policy candidate: real Tracker listen-window /
  radio duty-cycle qualification may require a different schedule.
- Uses wrap-safe unsigned monotonic subtraction. Revoked service,
  wrong radio identity, unavailable radio TX or no current grant
  never transmits or claims an ACK. Failed radio admission retains
  all volatile handles; actual OCQ2 storage is not deleted.
- When a different Tracker sends a custody object while a foreign
  group is queued, request flush for the prior group and
  **refuse to claim custody of the incompatible new object**. The
  Tracker must re-send it after the old group is transmitted.
  This is fail-closed single-group behavior; multi-Tracker
  fairness needs further qualification.
- Before group downlink TX, always try one newly received
  PRODUCT_SECURE frame. Ordinary **ACKless LIVE** is independently
  admitted to durable OCQ2 while a five-object ACK is pending or
  downlink is blocked. LIVE Flash writes and batch Flash writes
  are serialized; neither consumes the other's result.
- Preserve existing SF5L6 Edge bridge. While the group holds
  durably readback-verified handles, `Sf5l0EdgeHandoff::service(false)`
  still polls already-started maintenance but defers starting a
  **new reclaim** that might erase a page still needed for its
  outgoing full-digest MAC. Default `service()` behavior is unchanged.

## PC gate

`bash tools/ci/run_kq05_gateway_batch_runtime_host.sh`

This uses a **fake radio only** with actual `ProductCustodyStore`,
OCQ2 simulated NOR, `GatewayProductLiveIngress`,
`Kq05GatewayBatchCollector` and full SHA256/HMAC scope-checked signer.

Cases: immediate five-item/88B group; 1..4 canonical partial
56/64/72/80B, non-extending timeout; lost ACK duplicate
re-enrollment without another Flash write; pending full group
with no available radio TX while LIVE is still stored durably;
TX driver rejection and successful retry; withheld service/wrong
hardware ID and revoked signing grant; wrap-around timeout and
foreign Tracker flush. The prior OCQ2/HMAC parent tests run too.
A source contract checks Gateway boot guard, Edge reclaim isolation,
disabled-by-default opt-in and actual transport entrypoint.

## Still NOT proved

1. **Real firmware build**, size/RAM, real RAK SX126x TX followed by
   Tracker RX, authorized HMAC and durable OBS1 release on physical
   boards, and proper Tracker listening window for partial-timeout
   ACKs. The 2-minute timeout is **not a field-qualified number**.
2. LoRa duty cycle / legal airtime, energy/current measurements, RF
   loss/backpressure and partial power-loss during real NOR writes.
3. Multiple Tracker groups with fair scheduling; radio mailbox
   capacity and dropped custody when busy require on-air retry.
4. New LIVE packets still arriving at the requested reporting
   cadence when Gateway has five pending, weak RF, full Flash,
   0x0A revocations and stalled downlink. A PC host test alone
   cannot establish the field freshness SLA.
5. Real Gateway Edge/SQLite ingestion through the backend and app,
   signed per-Gateway credentials and final go/no-go gates.

**No physical uploads, no deletes, no legacy History erase.
P0 remains BLOCKED / NOT RELEASED.**
