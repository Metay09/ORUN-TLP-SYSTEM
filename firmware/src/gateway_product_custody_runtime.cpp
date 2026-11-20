#include "gateway_product_custody_runtime.h"

#include <string.h>
#ifdef ORUN_GATEWAY_BATCH_ACK
#include "monotonic_time.h"
#endif

namespace orun_tlp {

#ifdef ORUN_GATEWAY_BATCH_ACK
uint32_t GatewayProductCustodyRuntime::reservedPages() const {
  uint32_t pages = 0;
  for (const auto& peer : peers_)
    for (unsigned i = 0; i < peer.count; ++i)
      if (peer.handles[i].page < 32U) pages |= uint32_t(1) << peer.handles[i].page;
  return pages;
}

void GatewayProductCustodyRuntime::collectBatchHandle(uint32_t now) {
  ProductCustodyStore::Handle handle{};
  GatewayCustodyAckScope scope{};
  if (!ingress_.takeAuthorizedBatchHandle(handle, scope)) return;
  // One slot per enrolled collar. A revoked enrollment can be replaced
  // without mixing its pending references with its successor's receipt.
  PeerBatch* chosen = &peers_[0];
  for (auto& peer : peers_) {
    if (peer.scope.tracker_device_id == scope.tracker_device_id) {
      chosen = &peer;
      break;
    }
    if (!peer.scope.tracker_device_id ||
        (chosen->scope.tracker_device_id && now - peer.heard_ms > now - chosen->heard_ms))
      chosen = &peer;
  }
  auto& peer = *chosen;
  const bool changed = peer.scope.tracker_device_id != scope.tracker_device_id ||
                       peer.scope.gateway_device_id != scope.gateway_device_id ||
                       peer.scope.key_epoch != scope.key_epoch ||
                       peer.scope.grant_generation != scope.grant_generation ||
                       peer.scope.custody_floor != scope.custody_floor;
  if (changed) peer = PeerBatch{};
  peer.sparse = changed || monotonic::elapsed(pending_rx_ms_, peer.heard_ms, 300000U);
  peer.heard_ms = pending_rx_ms_;
  peer.scope = scope;
  if (!peer.count) {
    peer.first_ms = now;
    peer.urgent = false;
  }
  peer.urgent |= pending_event_;
  for (unsigned i = 0; i < peer.count; ++i) {
    const auto& existing = peer.handles[i];
    if (existing.page == handle.page && existing.slot == handle.slot &&
        existing.generation == handle.generation) {
      if (peer.count == 1 && monotonic::elapsed(now, peer.first_ms, 10000U)) peer.urgent = true;
      return;
    }
  }
  if (peer.count < kq05_gateway_batch_ack::kMaxObjects) peer.handles[peer.count++] = handle;
}
#endif

GatewayProductCustodyRuntime::Event
GatewayProductCustodyRuntime::update(bool enabled) {
  // Flash completion is owned by GatewayLocalCustodyOwner through ingress,
  // never by a radio callback. Always settle already-started writes even if
  // the service is withdrawn while a NOR mutation is in flight.
#ifdef ORUN_GATEWAY_BATCH_ACK
  if (live_ && live_->pending()) {
    const auto outcome = live_->service();
    if (outcome == GatewayProductLiveIngress::Service::kDurable) return Event::kLiveDurable;
    if (outcome == GatewayProductLiveIngress::Service::kFailed) return Event::kDenied;
    return Event::kNone;
  }
#endif
  if (ingress_.pending()) ingress_.service();

  // A legacy BASE receiver is not authorized to issue custody ACKs.
  // Gateway identity must be explicitly matched to the trusted admission
  // owner's locally configured identity before consuming a radio mailbox.
  if (!enabled || ingress_.localGatewayId() == 0U ||
      radio_.deviceId() != ingress_.localGatewayId()) {
#ifdef ORUN_GATEWAY_BATCH_ACK
      for (auto& peer : peers_) peer.count = 0;
      if (ingress_.batchReady()) {
        ProductCustodyStore::Handle unused{};
        GatewayCustodyAckScope scope{};
        ingress_.takeAuthorizedBatchHandle(unused, scope);
      }
#endif
    return Event::kNone;
  }
#ifdef ORUN_GATEWAY_BATCH_ACK
  const uint32_t now = monotonic::nowMs();
  if (ingress_.batchReady()) {
    collectBatchHandle(now);
    return Event::kLiveDurable;  // The Edge may take its own copy immediately.
  }
  // Drop volatile references if the collar has left. It retains unacknowledged records.
  for (auto& peer : peers_) {
    if (!peer.count) continue;
    if (monotonic::elapsed(now, peer.heard_ms, 600000U)) peer.count = 0;
    if (!peer.count || ingress_.pending() || command_window_reserved_ ||
        monotonic::elapsed(now, peer.heard_ms, 9000U)) continue;
    const bool due = peer.urgent || peer.count >= kq05_gateway_batch_ack::kMaxObjects - 1U ||
        (peer.sparse && monotonic::elapsed(now, peer.heard_ms, 1000U)) ||
        monotonic::elapsed(now, peer.first_ms, 300000U);
    if (due && radio_.canSend()) {
      const bool sent = radio_.sendAuthorizedBatchAck(ingress_, peer.handles, peer.count, peer.scope);
      peer.count = 0;
      if (sent) return Event::kAckTxStarted;
    }
  }
#endif

  // Never consume another PRODUCT_SECURE frame while a durable custody
  // operation / ACK is outstanding. Keep ACK admission serial with grant
  // revocation on this loop owner. RadioManager rechecks the grant itself
  // before its privileged ACK TX attempt.
  if (ingress_.pending()) {
#ifdef ORUN_GATEWAY_BATCH_ACK
    return Event::kNone;
#else
    return radio_.canSend() && radio_.sendAuthorizedCustodyAck(ingress_)
               ? Event::kAckTxStarted : Event::kNone;
#endif
  }

  ProductSecureRxFrame received{};
  if (!radio_.takeProductSecureFrame(&received)) return Event::kNone;

  // This mailbox provides only structural validation and RSSI/SNR. The
  // ingress must consult an actually provisioned per-tracker grant; its
  // store owner must durably commit and read back the exact opaque object.
#ifdef ORUN_GATEWAY_BATCH_ACK
  tlp::ProductSecureFrame header{};
  if (tlp::decodeProductSecureFrame(received.payload, received.size, &header) !=
      tlp::ProductSecureCodecResult::kOk) return Event::kDenied;
  if (!(header.path_flags & tlp::kProductPathFlagCustodyRequested)) {
    if (!live_) return Event::kDenied;
    const auto outcome = live_->receive(received.payload, received.size);
    if (outcome == GatewayProductLiveIngress::Receive::kStarted) {
      last_rssi_ = received.rssi_dbm;
      last_snr_ = received.snr_db;
      return Event::kCustodyStarted;
    }
    return outcome == GatewayProductLiveIngress::Receive::kDuplicateDurable
               ? Event::kLiveDurable : Event::kDenied;
  }
  pending_rx_ms_ = now;
  pending_event_ = header.app_family == tlp::kProductAppFamilyEvent;
  const auto outcome = ingress_.receive(received.payload, received.size, true);
#else
  const auto outcome = ingress_.receive(received.payload, received.size);
#endif
  memset(received.payload, 0, sizeof(received.payload));

  switch (outcome) {
    case GatewayCustodyAuthorizedIngress::Result::kStarted:
      last_rssi_ = received.rssi_dbm;
      last_snr_ = received.snr_db;
      return Event::kCustodyStarted;
    case GatewayCustodyAuthorizedIngress::Result::kAckReady:
#ifdef ORUN_GATEWAY_BATCH_ACK
      collectBatchHandle(now);
      return Event::kDuplicateHeld;
#else
      // Exact, independently read-back duplicate: one bounded ACK attempt
      // is eligible, but never if radio is busy. A failed TX does not imply
      // a delivery; the Tracker may retransmit the same durable object.
      if (radio_.canSend() &&
          radio_.sendAuthorizedCustodyAck(ingress_))
        return Event::kAckTxStarted;
      return Event::kDuplicateHeld;
#endif
    case GatewayCustodyAuthorizedIngress::Result::kDenied:
      return Event::kDenied;
    case GatewayCustodyAuthorizedIngress::Result::kBusy:
      // Race with an unavailable flash owner: drop the untrusted frame,
      // never ACK it. Tracker exact-byte retry remains the recovery path.
      return Event::kNone;
  }
  return Event::kNone;
}

}  // namespace orun_tlp
