#include "radio_manager.h"

#include <Arduino.h>
#include <SX126x-Arduino.h>
#include <string.h>

#include "gateway_custody_authorized_ingress.h"
#include "monotonic_time.h"
#include "radio_driver_gate.h"

namespace orun_tlp {

#ifdef ORUN_GATEWAY_BATCH_ACK
RadioManager::BatchTx RadioManager::sendAuthorizedBatchAck(
    GatewayCustodyAuthorizedIngress& ingress,
    const ProductCustodyStore::Handle* handles, uint8_t count,
    const GatewayCustodyAckScope& scope, uint32_t rx_floor_ms) {
  if (!canSend() || !device_id_ || ingress.localGatewayId() != device_id_) return BatchTx::kDeferred;
  if (downlink_result_pending_) return BatchTx::kDeferred;
  uint8_t wire[kq05_gateway_batch_ack::kMaxSize]{};
  size_t size = 0;
  bool stale_single = false;
  if (!ingress.prepareAuthorizedBatchAck(handles, count, scope, wire, size, &stale_single))
    return stale_single ? BatchTx::kStaleHandle : BatchTx::kUnverifiable;
  radio_driver::Guard gate;
  if (!gate || !canSend()) return BatchTx::kDeferred;
  if (downlink_result_pending_) return BatchTx::kDeferred;
  if (!startTxOperation(static_cast<uint8_t>(size), &rx_floor_ms)) return BatchTx::kDeferred;
  tx_kind_ = TxKind::kBatchAck;
  batch_ack_rx_floor_ms_ = rx_floor_ms;

  downlink_result_ = {};
  downlink_result_.kind = DownlinkTxResult::Kind::kAck;
  downlink_result_.target = scope.tracker_device_id;
  downlink_result_.rx_floor_ms = rx_floor_ms;
  tx_role_epoch_ = role_epoch_;
  rx_restore_state_ = RxRestoreState::kNone;
  ++tx_attempts_;
  ++event_diagnostics_.custody_ack_tx_attempts;
  const uint32_t before_send = monotonic::nowMs();
  Radio.Send(wire, static_cast<uint8_t>(size));
  tx_started_ms_ = monotonic::nowMs();
  const uint32_t send_call_ms = tx_started_ms_ - before_send;
  if (send_call_ms > event_diagnostics_.batch_ack_max_send_call_ms)
    event_diagnostics_.batch_ack_max_send_call_ms = send_call_ms;
  return BatchTx::kStarted;
}
#endif
}  // namespace orun_tlp
