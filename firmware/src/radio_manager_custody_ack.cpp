#include "radio_manager.h"

#include <Arduino.h>
#include <SX126x-Arduino.h>
#include <string.h>

#include "gateway_custody_authorized_ingress.h"
#include "monotonic_time.h"
#include "radio_driver_gate.h"

namespace orun_tlp {

// SF5K3T: privileged, single-loop-owner ACK radio transport. The trusted
// Gateway ingress performs durable exact-object readback, HMAC and a final
// grant recheck before yielding its one-shot ACK. This caller never accepts
// an arbitrary 56-byte radio frame from an untrusted application component.
// Serialize all grant changes, ingress calls and TX admission on the loop
// owner. No RX callback may mutate a grant or initiate this transmission.
// A denied/failed TX start consumes this one-shot ACK, not durable custody.
bool RadioManager::sendAuthorizedCustodyAck(
    GatewayCustodyAuthorizedIngress& ingress) {
  // Do not consume an authorized ACK while radio is knowingly unavailable.
  // Mobile Gateway operation is explicitly provisioned, not inferred from
  // the legacy BASE, RELAY or TRACKER role.
  if (!canSend() || device_id_ == 0U ||
      ingress.localGatewayId() != device_id_) return false;

  uint8_t ack[custody_ack::kWireSize]{};
  // Final live grant check; may use independent flash resources, so do not
  // hold radio_driver::Guard while calling it.
  if (!ingress.takeAuthorizedAck(ack)) return false;
  custody_ack::Header header{};
  if (!custody_ack::decodeHeader(ack, sizeof(ack), header) ||
      !header.relay_allowed ||
      header.gateway_device_id != device_id_) {
    memset(ack, 0, sizeof(ack));
    ++local_tx_failures_;
    return false;
  }

  radio_driver::Guard gate;
  if (!gate || !canSend()) {
    memset(ack, 0, sizeof(ack));
    ++local_tx_failures_;
    return false;
  }

  startTxOperation();
  tx_kind_ = TxKind::kCustodyAck;
  tx_role_epoch_ = role_epoch_;
  rx_restore_state_ = RxRestoreState::kNone;
  ++tx_attempts_;
  ++event_diagnostics_.custody_ack_tx_attempts;
  // The pinned SX126x-Arduino driver copies bytes synchronously to its FIFO.
  Radio.Send(ack, static_cast<uint8_t>(sizeof(ack)));
  tx_started_ms_ = monotonic::nowMs();
  memset(ack, 0, sizeof(ack));
  Serial.printf(
      "TX CUSTODY_ACK tracker=%08lX%08lX gateway=%08lX%08lX len=56 outcome=pending\n",
      static_cast<unsigned long>(uint32_t(header.tracker_device_id >> 32)),
      static_cast<unsigned long>(uint32_t(header.tracker_device_id)),
      static_cast<unsigned long>(uint32_t(device_id_ >> 32)),
      static_cast<unsigned long>(uint32_t(device_id_)));
  return true;  // Radio start, NOT receipt, custody release, or delivery.
}

}  // namespace orun_tlp
