#pragma once

#include <stdint.h>

#include "ble_application_transport.h"

namespace orun_tlp {

// M7P7G: fixed-memory callback/task -> loop handoff for the BLE application
// adapter. The class itself performs no locking; production wraps every
// cross-task call in the same taskENTER/EXIT_CRITICAL discipline already used
// by the BLE disconnect counter. No Bluefruit/Arduino dependency is allowed
// here so session/mailbox behavior stays host-testable.
struct BleApplicationIngressEvent {
  uint32_t session_generation = 0;
  uint16_t connection_handle = 0xFFFFU;
  uint8_t frame_len = 0;
  uint8_t frame[ble_app_transport::kMaxFrameSize]{};
};

struct BleApplicationConfirmationEvent {
  uint32_t session_generation = 0;
  uint16_t connection_handle = 0xFFFFU;
  uint16_t value_handle = 0xFFFFU;
};

// BLE_GATTS_EVT_TIMEOUT is terminal for ATT protocol progress on that
// connection. The BLE event task stamps only the current session/connection;
// loop() owns teardown and the physical disconnect request.
struct BleApplicationGattTimeoutEvent {
  uint32_t session_generation = 0;
  uint16_t connection_handle = 0xFFFFU;
};

namespace ble_app_handoff_config {
// One full M7P7F logical request can arrive as four ATT write-with-response
// fragments before the cooperative loop runs. Bound the callback queue to that
// exact protocol maximum so valid fragmentation does not depend on loop timing.
constexpr uint8_t kIngressQueueCapacity = ble_app_transport::kMaxFragments;
}  // namespace ble_app_handoff_config

class BleApplicationHandoff {
 public:
  // Activating a new session invalidates every queued event from the previous
  // one. generation==0 is rejected because M7P7F reserves zero.
  void activateSession(uint16_t connection_handle, uint32_t generation);

  // Exact-match only. A delayed cleanup from an older connection/session is a
  // no-op and cannot clear replacement-session work.
  void deactivateSession(uint16_t connection_handle, uint32_t generation);

  // Stop-and-wait gate mirrored into callback context. The loop closes this
  // gate as soon as BleApplicationTransport owns an outbound response and
  // reopens it only after the response is fully indication-confirmed.
  void setIngressAllowed(uint16_t connection_handle, uint32_t generation,
                         bool allowed);

  // Callback/task producer. FIFO capacity equals the M7P7F four-fragment
  // logical-message maximum. A fifth queued frame is rejected rather than
  // overwriting earlier events.
  bool enqueueIngress(uint16_t connection_handle, const uint8_t* frame,
                      uint16_t frame_len);

  // Loop consumer.
  bool takeIngress(BleApplicationIngressEvent& out);

  // Callback/task producer for BLE_GATTS_EVT_HVC. One confirmation maximum;
  // exact session/connection matching is stamped at enqueue time.
  bool enqueueConfirmation(uint16_t connection_handle, uint16_t value_handle);

  // Loop consumer.
  bool takeConfirmation(BleApplicationConfirmationEvent& out);

  // Callback/task producer for a protocol-source BLE_GATTS_EVT_TIMEOUT.
  // This is terminal for the current ATT session: callback ingress closes and
  // already-queued ingress/HVC facts are discarded immediately, while loop()
  // remains the sole owner of transport teardown and Bluefruit.disconnect().
  bool enqueueGattTimeout(uint16_t connection_handle);

  // Loop consumer.
  bool takeGattTimeout(BleApplicationGattTimeoutEvent& out);

  bool sessionActive() const { return session_active_; }
  bool ingressAllowed() const { return ingress_allowed_; }
  uint16_t connectionHandle() const { return connection_handle_; }
  uint32_t sessionGeneration() const { return session_generation_; }

 private:
  void clearIngressQueue();
  void clearMailboxes();

  bool session_active_ = false;
  bool ingress_allowed_ = false;
  uint16_t connection_handle_ = 0xFFFFU;
  uint32_t session_generation_ = 0;

  BleApplicationIngressEvent
      ingress_queue_[ble_app_handoff_config::kIngressQueueCapacity]{};
  uint8_t ingress_head_ = 0;
  uint8_t ingress_count_ = 0;

  bool confirmation_pending_ = false;
  BleApplicationConfirmationEvent confirmation_{};

  bool gatt_timeout_pending_ = false;
  BleApplicationGattTimeoutEvent gatt_timeout_{};
};

}  // namespace orun_tlp
