# SF5K3U — Tracker ACK receive to durable release integration

Base: `e9f137c030a280056db284b8e2fc787ece6fa88f` (validated SF5K3T).

This is a narrow production-source composition change, not a cutover:
- Main loop consumes structurally admitted, untrusted Tracker ACKs only after
  the radio callback handoff and on an already provisioned, ready security
  and mounted product store.
- Existing `CustodyAckReleaseOwner` verifies the actual SecurityStore v3
  grant, exact retained protected frame and full HMAC; it alone can request
  an ObservationStore release through ProductRecordStore.
- ProductRecordStore poll precedes the release service. Only confirmed durable
  release completion produces the `release=DURABLE` event.
- No automatic flash reset, physical partition reassignment or protocol change;
  existing legacy POSITION / SF3 RF transmission remains active.
- A received ACK during transient store/security unavailability may be dropped,
  but never used to release a record. Missing ACK requires retransmission.

Still blocked: actual Tracker PRODUCT_SECURE seal/cache/TX composition; durable
Gateway grant provisioning; independent Gateway storage ownership and
PRODUCT_SECURE RX/ACK TX main-loop activation; Edge handoff; RF hardware and
power-cut testing. No new direct/relay/batch ACK protocol is introduced.

Focused: `bash tools/ci/run_sf5k3u_tracker_ack_runtime_host.sh`
(includes actual SF5K3M HMAC/durable release/reset fake-NOR tests).
Full host: `bash firmware/tests/run_host_tests.sh`
Target: `pio run -d firmware -e rak4630`
All validation on this commit remains pending until executed.
