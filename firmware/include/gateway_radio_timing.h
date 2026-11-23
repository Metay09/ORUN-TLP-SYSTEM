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

// All TX kinds on the Gateway share this bucket. Cold boots earn credit
// from zero. In an hour: 342 s refill + 12 s burst + at most one frame
// straddling the left boundary (< 6 s for 255 B), below 360 s.
class GatewayAirtimeBudget {
 public:
  // One initial Tracker report. Never granted to a Gateway.
  // Cannot be granted twice, even after the credit is spent.
  void grantTrackerStartupCredit() {
    if (tracker_startup_initialized_) return;
    tracker_startup_initialized_ = true;
    if (last_ms_ == 0U && credit_us_ == 0U)
      credit_us_ = 2300000U;
  }

  bool available(uint32_t now, uint8_t size, uint32_t reserve_us = 0) {
    const uint32_t elapsed = now - last_ms_;
    last_ms_ = now;
    if (elapsed > kCapacityUs / kRefillUsPerMs) credit_us_ = kCapacityUs;
    else {
      credit_us_ += elapsed * kRefillUsPerMs;
      if (credit_us_ > kCapacityUs) credit_us_ = kCapacityUs;
    }
    return credit_us_ >= cost(size) + reserve_us;
  }
  void charge(uint8_t size) { credit_us_ -= cost(size); }

 private:
  static constexpr uint32_t kRefillUsPerMs = 95U;
  static constexpr uint32_t kCapacityUs = 12000000U;
  static uint32_t cost(uint8_t size) { return profileAirtimeUs(size) + 1000U; }
  uint32_t credit_us_ = 0;
  uint32_t last_ms_ = 0;
  bool tracker_startup_initialized_ = false;
};
}  // namespace orun_tlp
