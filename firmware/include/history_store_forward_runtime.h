#pragma once

#include <stdint.h>

#include "history_receipt_admission.h"
#include "history_secure_crypto.h"
#include "history_store.h"
#include "radio_manager.h"

namespace orun_tlp {

// First production device-side SF3 activation.
//
// One loop-owned runtime composes existing reviewed owners without changing
// their authority:
// - HistoryStore owns retained records/incarnation/RAM acknowledgement.
// - HistorySecureCrypto owns root-credential protection/open.
// - M4P5B owns A2D replay-before-delivery ordering.
// - RadioManager owns SX1262 TX/RX and callback handoff.
//
// Backlog replay is deliberately conservative in this first activation:
// one oldest-first direct HISTORY_SECURE probe per bounded interval, always
// below current PositionFlow/radio work. There is no persistent replay cursor
// and no per-record durable delivery checkpoint here.
class HistoryStoreForwardRuntime {
 public:
  static constexpr uint32_t kDefaultInitialProbeDelayMs = 15UL * 60UL * 1000UL;
  static constexpr uint32_t kDefaultProbeIntervalMs = 60UL * 60UL * 1000UL;
  static constexpr uint32_t kUnavailableRetryMs = 60UL * 1000UL;

  enum class Event : uint8_t {
    kNone,
    kReplaySent,
    kReplaySendFailed,
    kReceiptApplied,
    kReceiptDuplicate,
    kReceiptReplayRejected,
    kReceiptRejected,
    kReceiptAuthRejected,
    kReceiptInvalidFrame,
    kOpaqueObservationReceived,
  };

  struct Diagnostics {
    uint32_t replay_attempts = 0;
    uint32_t replay_sent = 0;
    uint32_t replay_local_failures = 0;
    uint32_t replay_crypto_unavailable = 0;
    uint32_t replay_invalid_records = 0;
    uint32_t receipt_frames = 0;
    uint32_t receipt_auth_rejections = 0;
    uint32_t receipt_invalid_frames = 0;
    uint32_t receipt_replay_rejections = 0;
    uint32_t receipt_applied = 0;
    uint32_t receipt_duplicates = 0;
    uint32_t receipt_delivery_rejections = 0;
    uint32_t opaque_observations_received = 0;
  };

  HistoryStoreForwardRuntime(
      HistoryStore& history,
      HistorySecureCrypto& crypto,
      HistoryReceiptAdmissionCoordinator& admission,
      RadioManager& radio,
      uint32_t initial_probe_delay_ms = kDefaultInitialProbeDelayMs,
      uint32_t probe_interval_ms = kDefaultProbeIntervalMs)
      : history_(history),
        crypto_(crypto),
        admission_(admission),
        radio_(radio),
        initial_probe_delay_ms_(initial_probe_delay_ms),
        probe_interval_ms_(probe_interval_ms) {}

  // Call once per application loop AFTER RadioManager::update().
  // higher_priority_pending must include current/live application work.
  Event update(uint32_t now_ms, bool higher_priority_pending);

  uint64_t lastReplayIdentity() const { return last_replay_identity_; }
  uint64_t lastReplaySecurityCounter() const {
    return last_replay_security_counter_;
  }
  const Diagnostics& diagnostics() const { return diagnostics_; }

 private:
  Event serviceAdmission();
  Event serviceReceivedFrame();
  Event serviceReplay(uint32_t now_ms, bool higher_priority_pending);
  bool scheduleAllowsProbe(uint32_t now_ms);
  void armNextProbe(uint32_t now_ms);

  HistoryStore& history_;
  HistorySecureCrypto& crypto_;
  HistoryReceiptAdmissionCoordinator& admission_;
  RadioManager& radio_;

  uint32_t initial_probe_delay_ms_;
  uint32_t probe_interval_ms_;
  uint32_t next_probe_at_ms_ = 0;
  bool probe_schedule_started_ = false;
  uint64_t last_replay_identity_ = 0;
  uint64_t last_replay_security_counter_ = 0;
  Diagnostics diagnostics_{};
};

}  // namespace orun_tlp
