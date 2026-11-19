#include "kq05_tracker_batch_radio_runtime.h"

#ifdef ORUN_KQ05_EXPERIMENTAL_GATEWAY_BATCH_RF
#include <string.h>

namespace orun_tlp {

void Kq05TrackerBatchRadioRuntime::clearRetry() {
  attempts_=0U;
  retry_at_ms_=0U;
  retry_armed_=false;
}

Kq05TrackerBatchRadioRuntime::Intake
Kq05TrackerBatchRadioRuntime::receive(bool allowed, uint32_t now_ms) {
  Kq05GatewayBatchAckRxFrame raw{};
  if (!radio_.peekKq05GatewayBatchAckFrame(&raw))
    return Intake::kNone;

  // Disabled, busy or high-priority LIVE handling is NEVER an excuse to
  // consume the pending bytes or block the unrelated position transmitter.
  if (!allowed || release_.pending()) return Intake::kDeferred;
  // uint32 wrap-safe for the bounded 5s retry delay.
  if (retry_armed_ &&
      static_cast<int32_t>(now_ms-retry_at_ms_) < 0)
    return Intake::kDeferred;

  // The exact 56..88 bytes enter only the previously PC-qualified
  // SecurityStore V3 + full-SHA256 HKDF/HMAC verifier. No structural
  // RadioManager admission is interpreted as proof of acceptance.
  const auto result=release_.submit(raw.payload,raw.size);
  memset(raw.payload,0,sizeof(raw.payload));

  if (result==Kq05TrackerBatchReleaseOwner::Submit::kBusy ||
      result==Kq05TrackerBatchReleaseOwner::Submit::kStorageUnavailable) {
    if (++attempts_ >= kMaxTemporaryAttempts) {
      // A permanently unavailable/reboot-stale mailbox must not pin every
      // subsequent authentic ACK forever. Gateway re-ACK on duplicate
      // durably accepted bytes can issue a fresh authenticated subset.
      radio_.discardKq05GatewayBatchAckFrame();
      clearRetry();
      return Intake::kRetryExpired;
    }
    retry_at_ms_=now_ms+kRetryDelayMs;
    retry_armed_=true;
    return Intake::kDeferred;
  }

  // kStarted only means authentication passed and the selective OBS1 release
  // state machine began; NOT durable release or radio delivery.
  radio_.discardKq05GatewayBatchAckFrame();
  clearRetry();
  return result==Kq05TrackerBatchReleaseOwner::Submit::kStarted
      ? Intake::kStarted : Intake::kRejected;
}

} // namespace orun_tlp
#endif  // ORUN_KQ05_EXPERIMENTAL_GATEWAY_BATCH_RF
