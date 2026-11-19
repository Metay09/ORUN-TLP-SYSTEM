# KQ05 — Tracker 0x0A Gateway batch ACK radio mailbox to OBS1 durable release

**Status: DRAFT, PC TEST NOT YET RUN.** Experimental feature is OFF in all
existing PlatformIO images unless explicitly built with
`ORUN_KQ05_EXPERIMENTAL_GATEWAY_BATCH_RF`. No device upload, RF field pilot,
migration, History erase, or release of old data.

**Parent PR #351:** user-confirmed PC PASS at
`40ae62f464ceb56494521b313071a8f55df62c39`:
real `RadioManager` Tracker untrusted type 0x0A RX/identity/role/88-byte
mailbox and private Gateway SX126x TX source compile, plus the previously
confirmed Gateway 1–5 OCQ2 collector and Tracker V3/HMAC/OBS1 release suite.

## New composition and invariants

- New opt-in `Kq05TrackerBatchRadioRuntime` takes a
  `RadioManager::peekKq05GatewayBatchAckFrame` copy **on the Tracker
  application task**, NOT in the radio callback.
- The 0x0A frame is never trusted by the radio transport. The only release
  path is `Kq05TrackerBatchReleaseOwner::submit()` with actual
  `SecurityStore V3` grant, scoped full digest HMAC, and five exact
  encrypted cached records; **no range delete**.
- A `kStarted` result is *not* delivery: `ProductRecordStore::poll()`
  must independently durably commit each OBS1 release marker before the
  event `kReleasedOne`; the group is only `kApplied` after all items.
- `main.cpp` instantiates one guarded owner/runtime and services marker
  completions **after** `product_records.poll()` while radio is idle.
  Crucially, no second release owner is admitted while the type 0x0A
  owner holds the shared Flash outcome. The existing one-record Gateway
  type 0x05 and Backend type 0x09 receipt paths remain intact.
- A busy SecurityStore/OBS1 or high-priority LIVE path leaves the exact
  raw 0x0A mailbox available for later re-try; no incoming ACK can
  suppress `pilot_allowed` or the ordinary LIVE sender. With an actual
  temporary storage-unavailability response, retry is delayed 5s
  and limited to 12 attempts; a permanently stale item eventually drops,
  allowing the Gateway to issue a fresh authenticated subset ACK on
  retransmission. The radio does not count this as success or delivery.
- Replayed/tampered/foreign/obsolete group ACKs cannot release any record
  without the current durable scope/HMAC and exact retained objects.
- The local one-frame mailbox may drop a second downlink while occupied:
  repeated durable duplicate object intake on the Gateway is necessary
  to recover a lost ACK. Multi-Gateway/fairness are NOT qualified.

## PC gate

`bash tools/ci/run_kq05_tracker_batch_radio_runtime_host.sh`

Runs source contract for disabled-by-default feature, unique 0x05/0x09
owners and unchanged LIVE priority; then compiles the actual Tracker
SecurityStore V3, full SHA256/HKDF/HMAC, ObservationStore and
ProductRecordStore against simulated NOR using only a fake radio
mailbox (the actual RadioManager RX source was independently qualified
in parent PR #351).

Test cases: no raw ACK -> no action; high-priority LIVE defers with no
Flash mutation; correct authenticated five-object ACK begins only
authorized release; five distinct OBS1 markers become durable; tampered
MAC rejected, no-grant rejected, a second ACK retained while the
previous group owns Flash, replay after release fails closed. Parent
selective release tests including reboot/power-cut and grant
revocation also run in this binary.

Other gates:
`bash tools/ci/run_kq05_gateway_batch_radio_transport_host.sh`
`bash tools/ci/run_kq05_gateway_batch_collector_host.sh`

## Still required before real pilot

1. Actual Gateway app-loop composition of the OCQ2 collector with a
   bounded deadline for partial 1–4 groups, RF TX admission and
   contention/backpressure across Tracker IDs. The present
   `GatewayProductCustodyRuntime` still follows the old per-record 0x05
   path; this change does not reduce airtime of running firmware.
2. Full firmware image compile of the *explicit opt-in configuration*,
   startup/role/provisioning audit and one physical Gateway + Tracker
   scoped grant test. The normal RAK build never enables this work.
3. Latest LIVE priority with lost downlink, an occupied five-object exact
   cache and continuous old backlog: prove new observations remain
   transmit-eligible and do not get stuck at five. An ACK capacity of 5
   is NOT itself proof of starvation freedom.
4. Power-loss while writing item n/5, fresh 4-item Gateway subset HMAC
   after reboot, RF duty-cycle/airtime, ACK TX fairness, actual RAK flash
   and energy measurements, and signed per-Gateway release credentials.

P0 / field release remains **BLOCKED**. No physical uploads or destructive
Flash operations without separate approval.
