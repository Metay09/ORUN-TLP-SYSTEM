# KQ05 — opt-in real SX126x 0x0A radio transport boundary

**Status:** DRAFT, PC test PENDING; feature macro disabled in normal product.
**Parent:** PR #350 `e70c699b7899f0ef3688d8ec993e02da567d3b08`,
owner-confirmed **PC PASS** for 1..5 actual Gateway OCQ2 durable-handle
collector and independently committed Tracker OBS1 release-marker chain.

## The code added

Experimental flag: `ORUN_KQ05_EXPERIMENTAL_GATEWAY_BATCH_RF`.

The default build does **not** define this flag. Normal RadioManager
single-record custody type 0x05, existing Backend receipt type 0x09,
live POSITION, store-forward and Gateway runtime remain unchanged.

- **Tracker receive:** actual `RadioManager::handleReceivedPacket()`
  recognizes canonical v2/type 0x0A frames of 56..88 bytes. Only a
  structurally valid ACK for the local Tracker ID enters its separate
  **one-frame raw mailbox**. It does not replace or overwrite the
  type 0x05 or Backend receipt mailbox.
- Copy the *entire exact* 56..88-byte frame out of the SX126x event buffer
  and retain its RSSI/SNR; reject wrong roles, foreign Tracker ID,
  malformed headers, wrong size, or a second frame while occupied.
- Clear the batch mailbox on role changes and radio reinitialization.
  The loop can `peek` until real `SecurityStore` / OBS1 is ready,
  `discard` rejected frames, or `take` for trusted verification.
- **Gateway TX:** only
  `RadioManager::sendAuthorizedKq05GatewayBatchAck(
  Kq05GatewayBatchCollector&)` may start an experimental 0x0A TX;
  there is **no raw public frame TX** function. The collector
  re-reads each OCQ2 durable handle, rechecks the current Gateway grant,
  and signs one scoped HMAC immediately before radio admission.
  `RadioManager` separately checks its local device identity,
  non-Tracker role, canonical wire and driver gate availability.
- Radio `Send()` is just SX126x TX initiation. Calling
  `collector.markTxAdmitted()` records ONLY successful local
  admission to the radio FIFO, not delivery, not Tracker custody,
  and not Backend confirmation. If the driver was unavailable before
  TX, the collector retains the group for a later attempt.
- The radio callback does **not** call a Flash mutation, HMAC,
  grant provisioning, or application release.
- `RadioEventDiagnostics` separately counts 0x0A RX queued/drops and
  0x0A TX start attempts, without conflating them with Backend receipts.

## PC verification

`bash tools/ci/run_kq05_gateway_batch_radio_transport_host.sh`

1. Compiles the actual `RadioManager`, its production wire decoder,
   and R2 fake SX126x driver under opt-in flag.
2. Exercises 88-byte Tracker RX exact-copy, retained mailbox,
   duplicate/drop, wrong role/ID, reserved flags, malformed length,
   role transition clearing and separation from normal PRODUCT RX.
3. Compiles the privileged Gateway SX126x TX implementation against
   host stubs under `-Wall -Wextra -Werror` with GNU C++17.
4. Actual Gateway collector and Tracker selective release host tests
   are separate preflight dependencies; both were previously PC PASS.

**No live Gateway-product-runtime or Tracker main() composition is
activated in this PR.** The feature flag itself is not turned on by the
default Arduino image and does not make a radio pilot ready.

## Next safety gates before real RF

- Explicitly integrate Gateway `Kq05GatewayBatchCollector` into the
  trusted Gateway boot-gated loop, including how to flush 1..4 records
  after a bounded real monotonic deadline, TX airtime budget and fair
  handling of other Tracker identities.
- Connect Tracker batch ACK mailbox to the already verified
  `Kq05TrackerBatchReleaseOwner` in the Tracker loop; NEVER release
  directly from the receive callback or on structural decode alone.
- Keep old type 0x05 per-critical-command/Event path, Backend 0x09
  receipts and routine ackless LIVE semantics separated.
- Introduce bounded retention and retries for rejected/temporarily
  busy ACK frames; a reboot after a partially completed five-record
  release requires a **new Gateway ACK for remaining records**.
- Production nRF/RAK image compile, measured RF air-time and current,
  post-reboot device Flash, two-device radio custody test, live-first
  fairness, supply/power loss and 14-day pilot are all **NOT DONE**.
- No firmware upload, physical flash erase or legacy 28KiB history
  cutover. P0 remains NOT RELEASED.
