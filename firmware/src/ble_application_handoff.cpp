#include "ble_application_handoff.h"

#include <string.h>

namespace orun_tlp {

void BleApplicationHandoff::clearIngressQueue() {
  for (uint8_t i = 0; i < ble_app_handoff_config::kIngressQueueCapacity; ++i)
    ingress_queue_[i] = BleApplicationIngressEvent();
  ingress_head_ = 0;
  ingress_count_ = 0;
}

void BleApplicationHandoff::clearMailboxes() {
  clearIngressQueue();
  confirmation_pending_ = false;
  confirmation_ = BleApplicationConfirmationEvent();
  gatt_timeout_pending_ = false;
  gatt_timeout_ = BleApplicationGattTimeoutEvent();
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
  // A protocol timeout is terminal until loop() tears the session down.
  // Never allow a late HVC/control update to reopen callback admission.
  if (gatt_timeout_pending_ && allowed) return;

  ingress_allowed_ = allowed;
  if (!allowed) {
    // Frames written while stop-and-wait is closed must never be retained
    // for later execution after the indication is confirmed.
    clearIngressQueue();
  }
}

bool BleApplicationHandoff::enqueueIngress(uint16_t connection_handle,
                                           const uint8_t* frame,
                                           uint16_t frame_len) {
  if (!session_active_ || !ingress_allowed_ || gatt_timeout_pending_ ||
      connection_handle != connection_handle_ ||
      frame_len > ble_app_transport::kMaxFrameSize ||
      (frame_len > 0 && frame == nullptr) ||
      ingress_count_ >= ble_app_handoff_config::kIngressQueueCapacity)
    return false;

  const uint8_t tail = static_cast<uint8_t>(
      (ingress_head_ + ingress_count_) %
      ble_app_handoff_config::kIngressQueueCapacity);
  BleApplicationIngressEvent& slot = ingress_queue_[tail];
  slot = BleApplicationIngressEvent();
  slot.session_generation = session_generation_;
  slot.connection_handle = connection_handle;
  slot.frame_len = static_cast<uint8_t>(frame_len);
  if (frame_len > 0) memcpy(slot.frame, frame, frame_len);
  ++ingress_count_;
  return true;
}

bool BleApplicationHandoff::takeIngress(BleApplicationIngressEvent& out) {
  if (ingress_count_ == 0) return false;
  out = ingress_queue_[ingress_head_];
  ingress_queue_[ingress_head_] = BleApplicationIngressEvent();
  ingress_head_ = static_cast<uint8_t>(
      (ingress_head_ + 1U) % ble_app_handoff_config::kIngressQueueCapacity);
  --ingress_count_;
  if (ingress_count_ == 0) ingress_head_ = 0;
  return true;
}

bool BleApplicationHandoff::enqueueConfirmation(uint16_t connection_handle,
                                                uint16_t value_handle) {
  if (!session_active_ || connection_handle != connection_handle_ ||
      confirmation_pending_ || gatt_timeout_pending_)
    return false;

  confirmation_.session_generation = session_generation_;
  confirmation_.connection_handle = connection_handle;
  confirmation_.value_handle = value_handle;
  confirmation_pending_ = true;

  // Wire confirmation has already happened. Provisionally accept post-HVC
  // request frames into the bounded FIFO even before loop() consumes this
  // confirmation. loop() always consumes HVC before ingress and will close/
  // clear the FIFO again if the confirmation does not match its in-flight
  // response, so no unconfirmed request can execute.
  ingress_allowed_ = true;
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

bool BleApplicationHandoff::enqueueGattTimeout(uint16_t connection_handle) {
  if (!session_active_ || connection_handle != connection_handle_ ||
      gatt_timeout_pending_)
    return false;

  // ATT protocol timeout makes further application progress invalid on this
  // connection. Close callback admission immediately and discard facts that
  // must not execute after loop-owned recovery begins.
  ingress_allowed_ = false;
  clearIngressQueue();
  confirmation_pending_ = false;
  confirmation_ = BleApplicationConfirmationEvent();

  gatt_timeout_.session_generation = session_generation_;
  gatt_timeout_.connection_handle = connection_handle;
  gatt_timeout_pending_ = true;
  return true;
}

bool BleApplicationHandoff::takeGattTimeout(
    BleApplicationGattTimeoutEvent& out) {
  if (!gatt_timeout_pending_) return false;
  out = gatt_timeout_;
  gatt_timeout_pending_ = false;
  gatt_timeout_ = BleApplicationGattTimeoutEvent();
  return true;
}

}  // namespace orun_tlp
