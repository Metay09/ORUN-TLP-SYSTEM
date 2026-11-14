#include "rf_duty_budget.h"

namespace orun_tlp {

uint32_t loraAirtimeMs(size_t payload_bytes,
                       const LoRaAirtimeProfile& p) {
  if (payload_bytes == 0U || payload_bytes > 255U ||
      p.spreading_factor < 7U || p.spreading_factor > 12U ||
      p.bandwidth_hz == 0U || p.coding_rate < 1U ||
      p.coding_rate > 4U || p.preamble_symbols == 0U)
    return 0U;

  const uint64_t symbol_us_times_bw =
      (uint64_t(1U) << p.spreading_factor) * 1000000ULL;
  const int32_t low_data_rate_opt =
      symbol_us_times_bw >= uint64_t(16000U) * p.bandwidth_hz ? 1 : 0;
  const int32_t numerator =
      int32_t(8U * payload_bytes) - 4 * int32_t(p.spreading_factor) +
      28 + (p.crc_enabled ? 16 : 0) - (p.explicit_header ? 0 : 20);
  const int32_t denominator =
      4 * (int32_t(p.spreading_factor) - 2 * low_data_rate_opt);
  const uint32_t blocks = numerator > 0
                              ? uint32_t((numerator + denominator - 1) /
                                         denominator)
                              : 0U;
  const uint32_t payload_symbols =
      8U + blocks * (4U + uint32_t(p.coding_rate));
  const uint64_t quarter_symbols = uint64_t(p.preamble_symbols) * 4U +
                                   17U + uint64_t(payload_symbols) * 4U;
  const uint64_t numerator_ms =
      quarter_symbols * (uint64_t(1U) << p.spreading_factor) * 1000ULL;
  const uint64_t denominator_ms = uint64_t(4U) * p.bandwidth_hz;
  return uint32_t((numerator_ms + denominator_ms - 1U) / denominator_ms);
}

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

RfDutyBudget::Decision RfDutyBudget::canReserve(
    uint32_t now_ms, uint32_t estimated_airtime_ms, Frame frame) {
  if (!started_) begin(now_ms);
  if (clock_fault_ || !stepClock(now_ms)) return Decision::kClockFault;
  if (static_cast<uint8_t>(frame) >
      static_cast<uint8_t>(Frame::kTest)) return Decision::kBadFrame;
  if (estimated_airtime_ms == 0U || estimated_airtime_ms > kMaxAirtimeMs)
    return Decision::kBadAirtime;

  const uint64_t current_minute = elapsed_ms_ / kBucketMs;
  const uint32_t used = totalInConservativeWindow(current_minute);
  used_at_current_minute_ = used;
  return estimated_airtime_ms > kMaxAirtimeMs - used
             ? Decision::kExhausted
             : Decision::kAllowed;
}

RfDutyBudget::Decision RfDutyBudget::reserve(
    uint32_t now_ms, uint32_t estimated_airtime_ms, Frame frame) {
  const Decision decision = canReserve(now_ms, estimated_airtime_ms, frame);
  if (decision != Decision::kAllowed) return decision;
  const uint64_t current_minute = elapsed_ms_ / kBucketMs;
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
