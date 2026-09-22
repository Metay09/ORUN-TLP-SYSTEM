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

  // Callback/task producer. One frame maximum; when occupied, the newer frame
  // is rejected rather than overwriting the earlier event.
  bool enqueueIngress(uint16_t connection_handle, const uint8_t* frame,
                      uint16_t frame_len);

  // Loop consumer.
  bool takeIngress(BleApplicationIngressEvent& out);

  // Callback/task producer for BLE_GATTS_EVT_HVC. One confirmation maximum;
  // exact session/connection matching is stamped at enqueue time.
  bool enqueueConfirmation(uint16_t connection_handle, uint16_t value_handle);

  // Loop consumer.
  bool takeConfirmation(BleApplicationConfirmationEvent& out);

  bool sessionActive() const { return session_active_; }
  bool ingressAllowed() const { return ingress_allowed_; }
  uint16_t connectionHandle() const { return connection_handle_; }
  uint32_t sessionGeneration() const { return session_generation_; }

 private:
  void clearMailboxes();

  bool session_active_ = false;
  bool ingress_allowed_ = false;
  uint16_t connection_handle_ = 0xFFFFU;
  uint32_t session_generation_ = 0;

  bool ingress_pending_ = false;
  BleApplicationIngressEvent ingress_{};

  bool confirmation_pending_ = false;
  BleApplicationConfirmationEvent confirmation_{};
};

}  // namespace orun_tlp
