#include "ble_application_handoff.h"

#include <string.h>

namespace orun_tlp {

void BleApplicationHandoff::clearMailboxes() {
  ingress_pending_ = false;
  ingress_ = BleApplicationIngressEvent();
  confirmation_pending_ = false;
  confirmation_ = BleApplicationConfirmationEvent();
}

void BleApplicationHandoff::activateSession(uint16_t connection_handle,
                                            uint32_t generation) {
  clearMailboxes();
  if (generation == 0) {
    session_active_ = false;
    ingress_allowed_ = false;
    connection_handle_ = 0xFFFFU;
    session_generation_ = 0;
    return;
  }

  session_active_ = true;
  ingress_allowed_ = true;
  connection_handle_ = connection_handle;
  session_generation_ = generation;
}

void BleApplicationHandoff::deactivateSession(uint16_t connection_handle,
                                              uint32_t generation) {
  if (!session_active_ || connection_handle != connection_handle_ ||
      generation != session_generation_)
    return;

  session_active_ = false;
  ingress_allowed_ = false;
  connection_handle_ = 0xFFFFU;
  session_generation_ = 0;
  clearMailboxes();
}

void BleApplicationHandoff::setIngressAllowed(uint16_t connection_handle,
                                              uint32_t generation,
                                              bool allowed) {
  if (!session_active_ || connection_handle != connection_handle_ ||
      generation != session_generation_)
    return;
  ingress_allowed_ = allowed;
  if (!allowed) {
    // A frame written while stop-and-wait is closed must never be retained
    // for later execution after the indication is confirmed.
    ingress_pending_ = false;
    ingress_ = BleApplicationIngressEvent();
  }
}

bool BleApplicationHandoff::enqueueIngress(uint16_t connection_handle,
                                           const uint8_t* frame,
                                           uint16_t frame_len) {
  if (!session_active_ || !ingress_allowed_ ||
      connection_handle != connection_handle_ ||
      frame_len > ble_app_transport::kMaxFrameSize ||
      (frame_len > 0 && frame == nullptr) || ingress_pending_)
    return false;

  ingress_.session_generation = session_generation_;
  ingress_.connection_handle = connection_handle;
  ingress_.frame_len = static_cast<uint8_t>(frame_len);
  if (frame_len > 0) memcpy(ingress_.frame, frame, frame_len);
  ingress_pending_ = true;
  return true;
}

bool BleApplicationHandoff::takeIngress(BleApplicationIngressEvent& out) {
  if (!ingress_pending_) return false;
  out = ingress_;
  ingress_pending_ = false;
  ingress_ = BleApplicationIngressEvent();
  return true;
}

bool BleApplicationHandoff::enqueueConfirmation(uint16_t connection_handle,
                                                uint16_t value_handle) {
  if (!session_active_ || connection_handle != connection_handle_ ||
      confirmation_pending_)
    return false;

  confirmation_.session_generation = session_generation_;
  confirmation_.connection_handle = connection_handle;
  confirmation_.value_handle = value_handle;
  confirmation_pending_ = true;
  return true;
}

bool BleApplicationHandoff::takeConfirmation(
    BleApplicationConfirmationEvent& out) {
  if (!confirmation_pending_) return false;
  out = confirmation_;
  confirmation_pending_ = false;
  confirmation_ = BleApplicationConfirmationEvent();
  return true;
}

}  // namespace orun_tlp
