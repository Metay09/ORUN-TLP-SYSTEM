#pragma once

#include "lora_airtime.h"
#include "radio_config.h"

namespace orun_tlp {

inline uint32_t profileAirtimeUs(uint8_t size) {
  using namespace radio_config;
  return estimateLoraAirtimeUs(size, kSpreadingFactor, kBandwidthKhz,
      kCodingRate, kPreambleLength, kCrcEnabled, kExplicitHeader);
}

// Margin for tick quantization, FIFO upload, PA ramp and scheduling after
// the final check. Verify this allowance at the antenna on the target.
constexpr uint32_t kAckTurnaroundGuardMs = 200U;
inline bool ackFitsWindow(uint32_t now, uint32_t heard, uint8_t size) {
  const uint32_t air_ms = (profileAirtimeUs(size) + 999U) / 1000U;
  return now - heard < radio_config::kWindowedRxAfterTxMs - air_ms - kAckTurnaroundGuardMs;
}


// One per-device RAM ledger. Never reset on a role change.
// Charge before physical TX, keep the charge until at least
// one hour after the latest possible end of that transmission.
class RfDutyBudget final {
 public:
  static constexpr uint32_t kHourMs = 3600000U;
  static constexpr uint32_t kMaxMs = 360000U;
  static constexpr uint32_t kMinuteMs = 60000U;
  static constexpr uint8_t kBuckets = 61U;

  static uint32_t costMs(uint8_t size) {
    if (!size) return 0U;
    return (profileAirtimeUs(size) + 1999U) / 1000U;
  }

  bool observe(uint32_t now) {
    if (fault_) return false;
    if (!started_) {
      started_ = true;
      last_ms_ = now;
      return true;
    }
    const uint32_t delta = now - last_ms_;
    if (delta >= 0x80000000UL) {
      fault_ = true;
      return false;
    }
    elapsed_ += delta;
    last_ms_ = now;
    return true;
  }

  bool available(uint32_t now, uint8_t size,
                 uint32_t reserve_us = 0U) {
    if (!observe(now) || active_ != kBuckets)
      return false;

    const uint32_t cost = costMs(size);
    if (!cost || !slotUsable(elapsed_)) return false;

    const uint64_t extra =
        (uint64_t(reserve_us) + 999U) / 1000U;

    return uint64_t(usedAt(elapsed_)) +
           cost + extra <= kMaxMs;
  }

  bool availableBeforeLive(
      uint32_t now, uint8_t size,
      uint32_t wait_ms, uint8_t live_size) {

    if (!available(now, size)) return false;

    const uint32_t live = costMs(live_size);
    if (!live) return false;

    const uint64_t future = elapsed_ + wait_ms;
    if (!slotUsable(future)) return false;

    const uint64_t current_minute =
        elapsed_ / kMinuteMs;
    const uint64_t future_minute =
        future / kMinuteMs;

    if (current_minute != future_minute &&
        current_minute % kBuckets ==
        future_minute % kBuckets &&
        future < elapsed_ + costMs(size) + kHourMs)
      return false;

    uint64_t projected = usedAt(future);

    const Bucket& current =
        bins_[current_minute % kBuckets];

    // A new TX can extend the lifetime of its entire bucket.
    // Do not forecast expiration of that bucket's older TXs.
    if (current.minute == current_minute &&
        !retained(current, future))
      projected += current.used;

    return projected + costMs(size) + live <= kMaxMs;
  }

  bool reserve(uint32_t now, uint8_t size) {
    if (!available(now, size)) return false;

    const uint64_t minute = elapsed_ / kMinuteMs;
    active_ = uint8_t(minute % kBuckets);

    Bucket& b = bins_[active_];

    if (b.minute != minute) {
      b = Bucket{};
      b.minute = minute;
    }

    const uint32_t cost = costMs(size);
    b.used += cost;

    const uint64_t end = elapsed_ + cost;
    if (end > b.last_end) b.last_end = end;

    return true;
  }

  void finish(uint32_t now) {
    if (!observe(now)) return;
    if (active_ == kBuckets) return;

    Bucket& b = bins_[active_];

    // Late TX_DONE/timeout observation extends retention.
    // Never refund a physically admitted transmission.
    if (elapsed_ > b.last_end)
      b.last_end = elapsed_;

    active_ = kBuckets;
  }

  uint32_t usedMs() const {
    return usedAt(elapsed_);
  }

  bool faulted() const { return fault_; }

 private:
  struct Bucket {
    uint64_t minute = UINT64_MAX;
    uint64_t last_end = 0U;
    uint32_t used = 0U;
  };

  Bucket bins_[kBuckets]{};
  uint64_t elapsed_ = 0U;
  uint32_t last_ms_ = 0U;
  uint8_t active_ = kBuckets;
  bool started_ = false;
  bool fault_ = false;

  static bool retained(const Bucket& b,
                       uint64_t now) {
    return b.used != 0U &&
           (now < b.last_end ||
            now - b.last_end < kHourMs);
  }

  uint32_t usedAt(uint64_t now) const {
    uint32_t total = 0U;
    for (const Bucket& b : bins_)
      if (retained(b, now)) total += b.used;

    return total;
  }

  bool slotUsable(uint64_t now) const {
    const uint64_t minute = now / kMinuteMs;
    const Bucket& b = bins_[minute % kBuckets];

    // Never overwrite a still-retained bucket on wrap.
    return b.minute == minute || !retained(b, now);
  }
};

}  // namespace orun_tlp
