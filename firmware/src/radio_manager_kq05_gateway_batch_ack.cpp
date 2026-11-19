#include "radio_manager.h"

#ifdef ORUN_KQ05_EXPERIMENTAL_GATEWAY_BATCH_RF

#include <Arduino.h>
#include <SX126x-Arduino.h>
#include <string.h>

#include "kq05_gateway_batch_collector.h"
#include "monotonic_time.h"
#include "radio_driver_gate.h"

namespace orun_tlp {

// The ONLY experimental type 0x0A Gateway batch ACK radio entrypoint:
// consumes a CURRENT grant/HMAC/durable-readback proposal from the
// privileged collector, never arbitrary radio-supplied bytes. This does
// not install a new Gateway role, trust raw product RX or grant authority.
bool RadioManager::sendAuthorizedKq05GatewayBatchAck(
    Kq05GatewayBatchCollector& collector) {
  if (!canSend() || device_id_ == 0U ||
      collector.localGatewayId() != device_id_ ||
      network_.role() == NodeRole::kTracker)
    return false;

  uint8_t bytes[kq05_gateway_batch_ack::kMaxSize]{};
  size_t length=0U;
  // This does readback, HMAC, and fresh grant checks OUTSIDE driver gate.
  // The owner must serialize grant mutations and this entire TX method
  // on the same Gateway loop task.
  if (collector.prepare(bytes,length) !=
          Kq05GatewayBatchCollector::Prepare::kReady)
    return false;

  kq05_gateway_batch_ack::Header parsed{};
  if (!kq05_gateway_batch_ack::decode(bytes,length,parsed) ||
      parsed.gateway_device_id != device_id_ ||
      length > UINT8_MAX) {
    memset(bytes,0,sizeof(bytes));
    ++local_tx_failures_;
    return false;
  }

  radio_driver::Guard gate;
  if (!gate || !canSend() ||
      collector.localGatewayId() != device_id_ ||
      network_.role() == NodeRole::kTracker) {
    memset(bytes,0,sizeof(bytes));
    ++local_tx_failures_;
    return false;  // Group stays staged for another authorized attempt.
  }

  startTxOperation();
  tx_kind_=TxKind::kCustodyAck;
  tx_role_epoch_=role_epoch_;
  rx_restore_state_=RxRestoreState::kNone;
  ++tx_attempts_;
  ++event_diagnostics_.custody_ack_tx_attempts;
  ++event_diagnostics_.kq05_gateway_batch_ack_tx_attempts;
  // The pinned SX126x-Arduino driver synchronously copies to its FIFO;
  // admission here is not proof of Tracker reception or release.
  Radio.Send(bytes,static_cast<uint8_t>(length));
  tx_started_ms_=monotonic::nowMs();

  const bool locally_admitted=collector.markTxAdmitted();
  memset(bytes,0,sizeof(bytes));
  if (!locally_admitted) {
    // Radio TX has started; do not claim safe local queue state.
    // This may only occur if the single-loop serialization contract was
    // broken. No additional group is silently sent or acknowledged here.
    ++local_tx_failures_;
    return false;
  }
  Serial.printf(
      "TX KQ05_GATEWAY_BATCH_ACK tracker=%08lX%08lX "
      "gateway=%08lX%08lX count=%u len=%u outcome=pending\\n",
      static_cast<unsigned long>(uint32_t(parsed.tracker_device_id >> 32)),
      static_cast<unsigned long>(uint32_t(parsed.tracker_device_id)),
      static_cast<unsigned long>(uint32_t(device_id_ >> 32)),
      static_cast<unsigned long>(uint32_t(device_id_)),
      static_cast<unsigned>(parsed.count),
      static_cast<unsigned>(length));
  return true;
}

} // namespace orun_tlp
#endif  // ORUN_KQ05_EXPERIMENTAL_GATEWAY_BATCH_RF
