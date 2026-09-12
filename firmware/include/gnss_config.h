#pragma once

#include <stdint.h>

namespace orun_tlp::gnss_config {

// RAK12500 official I2C example powers the sensor slot via WB_IO2, waits one
// second after each level change, and uses the default I2C address in begin().
constexpr uint32_t kPowerSettleMs = 1000;
constexpr uint8_t kI2cAddress = 0x42;
constexpr uint16_t kConfigurationMaxWaitMs = 250;
// Central M3 settings; no persistence or provisioning yet.
constexpr uint32_t kTrackingIntervalSeconds = 15 * 60;
constexpr uint32_t kAcquisitionTimeoutSeconds = 120;
constexpr uint32_t kShortIntervalThresholdSeconds = 60;
constexpr uint32_t kTrackingIntervalMs = kTrackingIntervalSeconds * 1000UL;
constexpr uint32_t kAcquisitionTimeoutMs = kAcquisitionTimeoutSeconds * 1000UL;
constexpr uint32_t kShortIntervalThresholdMs =
    kShortIntervalThresholdSeconds * 1000UL;
constexpr uint8_t kNavigationFrequencyHz = 1;
// M3 uses the official switched-slot power control. Avoid a power cycle when
// acquisition finishes just before the next due point, even at long intervals.
constexpr uint32_t kMinimumPowerOffMs = 2000;
constexpr uint32_t kFreshFixMaxAgeMs = 5000;

constexpr bool keepTracking(uint32_t interval_ms, uint32_t remaining_ms) {
  return interval_ms <= kShortIntervalThresholdMs ||
         remaining_ms <= kMinimumPowerOffMs;
}

static_assert(kTrackingIntervalSeconds > 0 &&
              kTrackingIntervalSeconds <= 12 * 24 * 60 * 60,
              "Interval must be positive and within M3's 12-day supported range");
static_assert(kAcquisitionTimeoutSeconds > 0 &&
              kAcquisitionTimeoutSeconds < 0x80000000UL / 1000,
              "Timeout must fit the monotonic half-range");
static_assert(kShortIntervalThresholdSeconds <= 12 * 24 * 60 * 60,
              "Short interval threshold must fit the supported interval range");

}  // namespace orun_tlp::gnss_config
