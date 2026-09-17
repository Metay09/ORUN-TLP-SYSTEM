#include "radio_manager.h"

#include "monotonic_time.h"
#include "radio_driver_gate.h"

namespace orun_tlp {

bool RadioManager::setRelayForwardingEnabled(bool enabled) {
  // Loop-owner only. A no-op is always safe, including while a TX is active.
  if (network_.relayForwardingEnabled() == enabled) return true;

  // Do not reinterpret in-flight work. The caller retries after the current
  // radio or legacy role transition reaches its normal terminal state.
  if (!ready_ || tx_in_progress_ || role_transition_pending_) return false;

  radio_driver::Guard gate;
  if (!gate) return false;

  // Consume any already-latched terminal work before deciding whether the
  // transport is idle enough for a synchronous behavior transition.
  orunRadioDispatchLocked();
  processCallbackEvents();
  if (tx_in_progress_ || role_transition_pending_) return false;

  // Gate ownership excludes dependency callbacks while quiesce clears pending
  // SX126x IRQ work. Drain already-handed-off RX events under the old behavior;
  // packets received before the transition must not be reinterpreted afterward.
  const uint32_t now = monotonic::nowMs();
  markRxStopped(now);
  orunRadioQuiesceLocked();
  processReceivedEvents();

  network_.setRelayForwardingEnabled(enabled);

  // Reuse the existing receive epoch to invalidate any delayed callback from
  // the previous network behavior without changing TLP bytes or queue policy.
  ++role_epoch_;
  armed_tx_generation_ = 0;
  radio_driver::setGeneration(0);

  rx_restore_state_ = RxRestoreState::kNone;
  listen_policy_ = desiredListenPolicy();
  if (listen_policy_ == RadioListenPolicy::kContinuous) {
    listen_state_ = RadioListenState::kRxContinuous;
    listen_window_deadline_ms_ = 0;
    requestRxRestore();
  } else {
    openListenWindow(now);
  }
  serviceRxRestore(now);
  return true;
}

}  // namespace orun_tlp
