#include "gateway_product_custody_runtime.h"

#include <string.h>

namespace orun_tlp {

GatewayProductCustodyRuntime::Event
GatewayProductCustodyRuntime::update(bool enabled) {
  // Flash completion is owned by GatewayLocalCustodyOwner through ingress,
  // never by a radio callback. Always settle already-started writes even if
  // the service is withdrawn while a NOR mutation is in flight.
  if (ingress_.pending()) ingress_.service();

  // A legacy BASE receiver is not authorized to issue custody ACKs.
  // Gateway identity must be explicitly matched to the trusted admission
  // owner's locally configured identity before consuming a radio mailbox.
  if (!enabled || ingress_.localGatewayId() == 0U ||
      radio_.deviceId() != ingress_.localGatewayId())
    return Event::kNone;

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
  const GatewayCustodyAuthorizedIngress::Result outcome =
      ingress_.receive(received.payload, received.size);
  memset(received.payload, 0, sizeof(received.payload));

  switch (outcome) {
    case GatewayCustodyAuthorizedIngress::Result::kStarted:
      last_rssi_ = received.rssi_dbm;
      last_snr_ = received.snr_db;
      return Event::kCustodyStarted;
    case GatewayCustodyAuthorizedIngress::Result::kAckReady:
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
