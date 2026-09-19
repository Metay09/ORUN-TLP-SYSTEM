#include "ble_admission_policy.h"

#include "monotonic_time.h"

namespace orun_tlp {

void BleAdmissionPolicy::begin(uint32_t now) {
  state_ = State::kOpen;
  connected_ = false;
  startFreshWindow(now);
}

void BleAdmissionPolicy::startFreshWindow(uint32_t now) {
  close_at_ms_ = now + ble_admission_config::kNoClientTimeoutMs;
  // Any physical action owed for this window (restart) is due immediately.
  next_action_at_ms_ = now;
}

void BleAdmissionPolicy::confirmClosed() {
  if (state_ == State::kClosing) state_ = State::kClosed;
}

BleAdmissionAction BleAdmissionPolicy::update(const BleAdmissionInput& input,
                                              uint32_t now) {
  if (state_ == State::kClosed) {
    // Confirmed closed (or never opened). Do not reopen implicitly -- a
    // future explicit, authenticated reopen path (e.g. LoRa OPEN_BLE) is out
    // of scope for this slice; see docs/milestones/M7P7B.md.
    connected_ = false;
    return BleAdmissionAction::kNone;
  }

  if (input.disconnect_event) {
    // A real disconnect happened since the last tick, even if the whole
    // connection fit between two polls. Grant one fresh bounded window and,
    // if a close was pending, cancel it: the client won the race.
    state_ = State::kOpen;
    startFreshWindow(now);
  }

  if (input.connected) {
    // Never closes while a client is connected, regardless of how close the
    // window was to expiring. A pending close is cancelled; the fresh window
    // follows the real disconnect.
    state_ = State::kOpen;
    connected_ = true;
    return BleAdmissionAction::kNone;
  }

  if (connected_) {
    // Polled edge: was connected last tick, is not now. Open exactly one
    // fresh bounded window -- repeated connect/disconnect cycles each grant
    // one more bounded window, never an unbounded/permanent one.
    connected_ = false;
    state_ = State::kOpen;
    startFreshWindow(now);
  }

  if (state_ == State::kOpen && monotonic::reached(now, close_at_ms_)) {
    state_ = State::kClosing;
    next_action_at_ms_ = now;
  }

  if (state_ == State::kClosing) {
    // Repeat until the caller confirms the physical stop, throttled.
    if (!monotonic::reached(now, next_action_at_ms_)) return BleAdmissionAction::kNone;
    next_action_at_ms_ = now + ble_admission_config::kRetryIntervalMs;
    return BleAdmissionAction::kClose;
  }

  if (!input.advertising_running && monotonic::reached(now, next_action_at_ms_)) {
    // Open and nobody connected, but nothing is advertising (e.g. the
    // framework's own restart is disabled, or a start failed): ask again,
    // throttled, for as long as the window stays open.
    next_action_at_ms_ = now + ble_admission_config::kRetryIntervalMs;
    return BleAdmissionAction::kStartAdvertising;
  }
  return BleAdmissionAction::kNone;
}

}  // namespace orun_tlp
