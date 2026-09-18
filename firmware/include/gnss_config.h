#pragma once

#include <stdint.h>

namespace orun_tlp::gnss_config {

// RAK12500 official I2C example powers the sensor slot via WB_IO2, waits one
// second after each level change, and uses the default I2C address in begin().
constexpr uint32_t kPowerSettleMs = 1000;
constexpr uint8_t kI2cAddress = 0x42;
constexpr uint16_t kConfigurationMaxWaitMs = 250;
// Central M3 settings; no persistence or provisioning yet.
// Development default (product-owner decision, M6P1 closure): 3 minutes
// while the project is in active development/test. This is a compile-time
// default change, not runtime/application configurability; see
// docs/milestones/M6P1.md for scope, rationale and physical evidence.
constexpr uint32_t kTrackingIntervalSeconds = 3 * 60;
// M7P5: durable config may override kTrackingIntervalSeconds at runtime
// (GnssManager::setTrackingIntervalMs). This bound is the same safety limit
// already enforced by the static_assert below; ConfigStore reuses it to
// validate a candidate interval before ever persisting or applying it, so
// there is exactly one source of truth for the supported range.
constexpr uint32_t kMaxTrackingIntervalSeconds = 12 * 24 * 60 * 60;
constexpr uint32_t kAcquisitionTimeoutSeconds = 120;
constexpr uint32_t kShortIntervalThresholdSeconds = 60;
constexpr uint32_t kTrackingIntervalMs = kTrackingIntervalSeconds * 1000UL;
constexpr uint32_t kAcquisitionTimeoutMs = kAcquisitionTimeoutSeconds * 1000UL;
constexpr uint32_t kShortIntervalThresholdMs =
    kShortIntervalThresholdSeconds * 1000UL;
constexpr uint8_t kNavigationFrequencyHz = 1;
// Preserve 2.2.29's effective 100ms poll interval for 1Hz NAV configuration.
constexpr uint8_t kI2cPollingWaitMs = 100;
// Bound repeated in-acquisition bus recovery so a broken sensor cannot keep an
// acquisition alive forever. Detection has its own three-attempt policy.
constexpr uint8_t kMaxI2cRecoveriesPerAcquisition = 2;
// M3 uses the official switched-slot power control. Avoid a power cycle when
// acquisition finishes just before the next due point, even at long intervals.
constexpr uint32_t kMinimumPowerOffMs = 2000;
// Age is strictly less than this limit, from each callback (not pair match).
constexpr uint32_t kFreshFixMaxAgeMs = 5000;
// Three boot detection attempts total, with 5s then 10s powered backoff.
// After exhaustion stay absent until reboot; no periodic absent-device wake.
constexpr uint8_t kDetectionMaxAttempts = 3;
constexpr uint32_t kDetectionRetryBackoffMs = 5000;

constexpr bool keepTracking(uint32_t interval_ms, uint32_t remaining_ms) {
  return interval_ms <= kShortIntervalThresholdMs ||
         remaining_ms <= kMinimumPowerOffMs;
}

static_assert(kTrackingIntervalSeconds > 0 &&
              kTrackingIntervalSeconds <= kMaxTrackingIntervalSeconds,
              "Interval must be positive and within M3's 12-day supported range");
static_assert(kAcquisitionTimeoutSeconds > 0 &&
              kAcquisitionTimeoutSeconds < 0x80000000UL / 1000,
              "Timeout must fit the monotonic half-range");
static_assert(kShortIntervalThresholdSeconds <= 12 * 24 * 60 * 60,
              "Short interval threshold must fit the supported interval range");
static_assert(kMaxI2cRecoveriesPerAcquisition > 0,
              "At least one bounded I2C recovery is required");

}  // namespace orun_tlp::gnss_config
