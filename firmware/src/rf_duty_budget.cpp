#include "rf_duty_budget.h"

namespace orun_tlp {

void RfDutyBudget::begin(uint32_t now_ms) {
  for (uint8_t i = 0U; i < kBucketCount; ++i) bins_[i] = Bucket{};
  elapsed_ms_ = 0U;
  last_ms_ = now_ms;
  used_at_current_minute_ = 0U;
  clock_fault_ = false;
  started_ = true;
}

bool RfDutyBudget::stepClock(uint32_t now_ms) {
  const uint32_t delta = now_ms - last_ms_;
  // Unsigned subtraction handles the ordinary millis() wrap. An elapsed
  // interval >= 2^31 ms is ambiguous (could be a backward time jump).
  if (delta >= 0x80000000UL) {
    clock_fault_ = true;
    return false;
  }
  last_ms_ = now_ms;
  elapsed_ms_ += delta;
  return true;
}

uint32_t RfDutyBudget::totalInConservativeWindow(
    uint64_t current_minute) const {
  uint32_t sum = 0U;
  for (uint8_t i = 0U; i < kBucketCount; ++i) {
    const Bucket& bucket = bins_[i];
    if (bucket.minute == UINT64_MAX || current_minute < bucket.minute ||
        current_minute - bucket.minute >= kBucketCount) continue;
    sum += bucket.airtime_ms;
  }
  return sum;
}

RfDutyBudget::Decision RfDutyBudget::reserve(
    uint32_t now_ms, uint32_t estimated_airtime_ms, Frame frame) {
  if (!started_) begin(now_ms);
  if (clock_fault_ || !stepClock(now_ms)) return Decision::kClockFault;
  if (static_cast<uint8_t>(frame) >
      static_cast<uint8_t>(Frame::kTest)) return Decision::kBadFrame;
  if (estimated_airtime_ms == 0U || estimated_airtime_ms > kMaxAirtimeMs)
    return Decision::kBadAirtime;
  // Development policy: budget is immediately available at boot.
  // No previous-boot usage is carried forward. Not field-compliance proof.

  const uint64_t current_minute = elapsed_ms_ / kBucketMs;
  const uint32_t used = totalInConservativeWindow(current_minute);
  used_at_current_minute_ = used;
  if (estimated_airtime_ms > kMaxAirtimeMs - used)
    return Decision::kExhausted;

  Bucket& bucket = bins_[current_minute % kBucketCount];
  if (bucket.minute != current_minute) {
    bucket.minute = current_minute;
    bucket.airtime_ms = 0U;
  }
  bucket.airtime_ms += estimated_airtime_ms;
  used_at_current_minute_ += estimated_airtime_ms;
  return Decision::kAllowed;
}

uint32_t RfDutyBudget::usedMs() const {
  return used_at_current_minute_;
}

}  // namespace orun_tlp
