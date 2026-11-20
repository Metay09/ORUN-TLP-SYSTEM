#include "gateway_product_custody_runtime.h"

#include <string.h>
#ifdef ORUN_EXPERIMENTAL_BATCH_ACK
#include "monotonic_time.h"
// An explicit bench peer selects the batch path. Other enrolled collars use 0x05.
#ifndef ORUN_BATCH_ACK_TRACKER_ID
#error "Select a paired lab tracker ID for the batch test build"
#endif
#endif

namespace orun_tlp {

#ifdef ORUN_EXPERIMENTAL_BATCH_ACK
uint32_t GatewayProductCustodyRuntime::reservedPages() const {
  uint32_t pages = 0;
  for (unsigned i = 0; i < count_; ++i)
    if (handles_[i].page < 32U) pages |= uint32_t(1) << handles_[i].page;
  return pages;
}

void GatewayProductCustodyRuntime::collectBatchHandle(uint32_t now) {
  ProductCustodyStore::Handle handle{};
  GatewayCustodyAckScope scope{};
  if (!ingress_.takeAuthorizedBatchHandle(handle, scope)) return;
  if (count_ && (batch_scope_.key_epoch != scope.key_epoch ||
                 batch_scope_.grant_generation != scope.grant_generation ||
                 batch_scope_.custody_floor != scope.custody_floor)) count_ = 0;
  if (!count_) {
    batch_scope_ = scope;
    first_handle_ms_ = now;
    urgent_ = false;
  }
  urgent_ |= pending_event_;
  for (unsigned i = 0; i < count_; ++i) {
    if (handles_[i].page == handle.page && handles_[i].slot == handle.slot &&
        handles_[i].generation == handle.generation) {
      // A lone retained record reoffered in a later receive window can be
      // acknowledged now rather than forcing another identical retry.
      if (count_ == 1 && monotonic::elapsed(now, first_handle_ms_, 10000U)) urgent_ = true;
      return;
    }
  }
  if (count_ < kq05_gateway_batch_ack::kMaxObjects) handles_[count_++] = handle;
}
#endif

GatewayProductCustodyRuntime::Event
GatewayProductCustodyRuntime::update(bool enabled) {
  // Flash completion is owned by GatewayLocalCustodyOwner through ingress,
  // never by a radio callback. Always settle already-started writes even if
  // the service is withdrawn while a NOR mutation is in flight.
#ifdef ORUN_EXPERIMENTAL_BATCH_ACK
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
#ifdef ORUN_EXPERIMENTAL_BATCH_ACK
      count_ = 0;
      if (ingress_.batchReady()) {
        ProductCustodyStore::Handle unused{};
        GatewayCustodyAckScope scope{};
        ingress_.takeAuthorizedBatchHandle(unused, scope);
      }
#endif
    return Event::kNone;
  }
#ifdef ORUN_EXPERIMENTAL_BATCH_ACK
  const uint32_t now = monotonic::nowMs();
  if (ingress_.batchReady()) {
    collectBatchHandle(now);
    return Event::kLiveDurable;  // The Edge may take its own copy immediately.
  }
  // Drop volatile references if the collar has left. It retains unacknowledged records.
  if (count_ && monotonic::elapsed(now, heard_batch_peer_ms_, 600000U)) count_ = 0;

  if (count_ && !ingress_.pending() &&
      (urgent_ || count_ == kq05_gateway_batch_ack::kMaxObjects ||
       monotonic::elapsed(now, first_handle_ms_, 300000U)) &&
      !monotonic::elapsed(now, heard_batch_peer_ms_, 9000U) &&
      radio_.canSend() && !command_window_reserved_) {
    const bool sent = radio_.sendAuthorizedBatchAck(ingress_, handles_, count_, batch_scope_);
    // A stale/reclaimed handle or a changed grant cannot acknowledge anything.
    // The collar still retains all unconfirmed records and can reoffer them.
    count_ = 0;
    if (sent) return Event::kAckTxStarted;
  }
#endif

  // Never consume another PRODUCT_SECURE frame while a durable custody
  // operation / ACK is outstanding. Keep ACK admission serial with grant
  // revocation on this loop owner. RadioManager rechecks the grant itself
  // before its privileged ACK TX attempt.
  if (ingress_.pending()) {
    return radio_.canSend() && radio_.sendAuthorizedCustodyAck(ingress_)
               ? Event::kAckTxStarted : Event::kNone;
  }

  ProductSecureRxFrame received{};
  if (!radio_.takeProductSecureFrame(&received)) return Event::kNone;

  // This mailbox provides only structural validation and RSSI/SNR. The
  // ingress must consult an actually provisioned per-tracker grant; its
  // store owner must durably commit and read back the exact opaque object.
#ifdef ORUN_EXPERIMENTAL_BATCH_ACK
  tlp::ProductSecureFrame header{};
  const bool batch = tlp::decodeProductSecureFrame(received.payload, received.size, &header) ==
                     tlp::ProductSecureCodecResult::kOk &&
                     header.device_id == ORUN_BATCH_ACK_TRACKER_ID;
  if (batch) heard_batch_peer_ms_ = now;
  if (batch && !(header.path_flags & tlp::kProductPathFlagCustodyRequested)) {
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
  pending_event_ = batch && header.app_family == tlp::kProductAppFamilyEvent;
  const auto outcome = ingress_.receive(received.payload, received.size, batch);
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
#ifdef ORUN_EXPERIMENTAL_BATCH_ACK
      if (batch) {
        collectBatchHandle(now);
        return Event::kDuplicateHeld;
      }
#endif
      // Exact, independently read-back duplicate: one bounded ACK attempt
      // is eligible, but never if radio is busy. A failed TX does not imply
      // a delivery; the Tracker may retransmit the same durable object.
      if (radio_.canSend() &&
          radio_.sendAuthorizedCustodyAck(ingress_))
        return Event::kAckTxStarted;
      return Event::kDuplicateHeld;
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
