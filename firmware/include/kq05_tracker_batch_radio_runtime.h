#pragma once

// KQ05 experimental 0x0A Gateway ACK -> real SecurityStore/OBS1 owner.
// Compile and activate ONLY on a deliberately selected pilot image.
// Default firmware continues using its existing 0x05 and 0x09 paths.
#ifdef ORUN_KQ05_EXPERIMENTAL_GATEWAY_BATCH_RF

#include <stdint.h>

#include "kq05_tracker_batch_release_owner.h"
#include "radio_manager.h"

namespace orun_tlp {

// Application-loop-only intake of UNTRUSTED exact raw radio 0x0A bytes.
// No direct Flash release, no cached HMAC authority in the radio owner.
// The composition root must serialize this adapter against single-object
// Gateway ACK and Backend receipt owners, and call the release owner only
// after the shared ProductRecordStore::poll() has driven Flash outcomes.
class Kq05TrackerBatchRadioRuntime final {
 public:
  enum class Intake : uint8_t {
    kNone, kStarted, kRejected, kDeferred, kRetryExpired
  };

  Kq05TrackerBatchRadioRuntime(RadioManager& radio,
                               Kq05TrackerBatchReleaseOwner& release)
      : radio_(radio), release_(release) {}

  // Only 'allowed' from the trusted loop can authorize an attempt. When
  // live/critical traffic or a different Flash owner is active, leave the
  // mailbox untouched, but NEVER gate the live radio or product sender.
  Intake receive(bool allowed, uint32_t now_ms);

  uint8_t temporaryAttempts() const { return attempts_; }

 private:
  void clearRetry();
  RadioManager& radio_;
  Kq05TrackerBatchReleaseOwner& release_;
  uint8_t attempts_ = 0U;
  uint32_t retry_at_ms_ = 0U;
  bool retry_armed_ = false;

  static constexpr uint8_t kMaxTemporaryAttempts = 12U;
  static constexpr uint32_t kRetryDelayMs = 5000U;
};

} // namespace orun_tlp
#endif  // ORUN_KQ05_EXPERIMENTAL_GATEWAY_BATCH_RF
