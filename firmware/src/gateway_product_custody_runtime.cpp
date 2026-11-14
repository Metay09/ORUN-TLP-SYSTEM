#include "gateway_product_custody_runtime.h"

#include <string.h>
#ifdef ORUN_KQ05_EXPERIMENTAL_ACKLESS_LIVE
#include "tlp_v2_product_secure.h"
#endif

namespace orun_tlp {

GatewayProductCustodyRuntime::Event
GatewayProductCustodyRuntime::update(bool enabled) {
  // Always finish any started custody mutation, even with service disabled.
  if (ingress_.pending()) ingress_.service();
#ifdef ORUN_KQ05_EXPERIMENTAL_ACKLESS_LIVE
  if (live_ != nullptr && live_->pending()) {
    const auto result = live_->service();
    if (result == GatewayProductLiveIngress::Service::kDurable)
      return Event::kLiveDurable;
    if (result == GatewayProductLiveIngress::Service::kFailed)
      return Event::kLiveDenied;
  }
#endif

  if (!enabled || ingress_.localGatewayId() == 0U ||
      radio_.deviceId() != ingress_.localGatewayId())
    return Event::kNone;

  // Previously every pending custody ACK stopped ALL product RX. The optional
  // KQ-05 path allows independent LIVE records when flash is no longer busy.
  if (ingress_.pending()) {
    if (radio_.canSend() && radio_.sendAuthorizedCustodyAck(ingress_))
      return Event::kAckTxStarted;
#ifndef ORUN_KQ05_EXPERIMENTAL_ACKLESS_LIVE
    return Event::kNone;
#else
    if (live_ == nullptr || live_->storageBusy())
      return Event::kNone;
#endif
  }

#ifdef ORUN_KQ05_EXPERIMENTAL_ACKLESS_LIVE
  if (live_ != nullptr && live_->storageBusy()) return Event::kNone;
#endif

  ProductSecureRxFrame received{};
  if (!radio_.takeProductSecureFrame(&received)) return Event::kNone;

#ifdef ORUN_KQ05_EXPERIMENTAL_ACKLESS_LIVE
  tlp::ProductSecureFrame decoded{};
  if (live_ != nullptr &&
      tlp::decodeProductSecureFrame(
          received.payload, received.size, &decoded) ==
          tlp::ProductSecureCodecResult::kOk &&
      (decoded.path_flags & tlp::kProductPathFlagCustodyRequested) == 0U) {
    const auto outcome = live_->receive(received.payload, received.size);
    memset(received.payload, 0, sizeof(received.payload));
    switch (outcome) {
      case GatewayProductLiveIngress::Receive::kStarted:
        return Event::kLiveStarted;
      case GatewayProductLiveIngress::Receive::kDuplicateDurable:
        return Event::kLiveDuplicate;
      case GatewayProductLiveIngress::Receive::kBusy:
        return Event::kNone;  // Do not claim delivery or send ACK.
      case GatewayProductLiveIngress::Receive::kDenied:
        return Event::kLiveDenied;
    }
  }
#endif

  const auto outcome = ingress_.receive(received.payload, received.size);
  memset(received.payload, 0, sizeof(received.payload));

  switch (outcome) {
    case GatewayCustodyAuthorizedIngress::Result::kStarted:
      last_rssi_ = received.rssi_dbm;
      last_snr_ = received.snr_db;
      return Event::kCustodyStarted;
    case GatewayCustodyAuthorizedIngress::Result::kAckReady:
      if (radio_.canSend() && radio_.sendAuthorizedCustodyAck(ingress_))
        return Event::kAckTxStarted;
      return Event::kDuplicateHeld;
    case GatewayCustodyAuthorizedIngress::Result::kDenied:
      return Event::kDenied;
    case GatewayCustodyAuthorizedIngress::Result::kBusy:
      return Event::kNone;
  }
  return Event::kNone;
}

}  // namespace orun_tlp
