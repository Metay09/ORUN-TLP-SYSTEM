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
// After exhaustion stay absent until reboot, unless setRedetectIntervalMs()
// asks for periodic re-detection (tracking explicitly requested).
constexpr uint8_t kDetectionMaxAttempts = 3;
constexpr uint32_t kDetectionRetryBackoffMs = 5000;

// Failure backoff (owner decision 2026-10-10): after this many consecutive
// acquisitions without a fresh fix (timeout or failure), the wait before the
// next one doubles each time, from 2x the interval up to 30 minutes, counted
// from the end of the failed attempt. The first fresh fix ends it. A receiver
// that cannot see the sky (barn, shed) then stops burning 120 s of GNSS every
// interval. Intervals of 30 minutes or more are never stretched.
constexpr uint8_t kFailureBackoffAfter = 2;
constexpr uint32_t kFailureBackoffMaxMs = 30UL * 60UL * 1000UL;

// 0 = no backoff; otherwise the wait from the end of the failed attempt.
// inline, not constexpr: the RAK toolchain builds this header as gnu++11.
inline uint32_t failureBackoffMs(uint32_t interval_ms,
                                    uint8_t consecutive_failures) {
  if (interval_ms == 0 || interval_ms >= kFailureBackoffMaxMs ||
      consecutive_failures < kFailureBackoffAfter)
    return 0;
  uint32_t wait = interval_ms;
  for (uint8_t i = 1; i < consecutive_failures && wait < kFailureBackoffMaxMs;
       ++i)
    wait = wait > kFailureBackoffMaxMs / 2 ? kFailureBackoffMaxMs : wait * 2;
  return wait < kFailureBackoffMaxMs ? wait : kFailureBackoffMaxMs;
}

// Backup sleep (owner decision 2026-10-11). When the next attempt is less
// than this far away, the receiver goes into u-blox backup (UBX-RXM-PMREQ
// with a timed wake) instead of having its rail cut, so it keeps time,
// position and ephemeris for a hot start (~1-2 s instead of ~26-30 s at
// 28-45 mA). Ephemeris ages out after about 4 h, after which a cut rail
// (0 uA) is cheaper than ~15 uA of backup current. GnssManager starts with
// backup sleep off; the composition root sets the default below.
constexpr uint32_t kBackupSleepMaxWaitMs = 4UL * 60UL * 60UL * 1000UL;
// Off by default (owner decision 2026-10-11, after the collar A/B): in the same
// place and hour, with the 3-minute interval, TTFF after backup sleep was no
// better than after a rail cut (median 22 s, 15-68 s, against 24 s, 13-80 s;
// 9 and 10 fixes, no timeouts). No hot start was seen on this RAK12500, so the
// ~15 uA backup current buys nothing and the rail is cut as before. The RAM-only
// USB "GNSS SLEEP <minutes>" still enables it for tests on other boards.
constexpr uint32_t kBackupSleepDefaultMaxWaitMs = 0;
// The receiver wakes this much before the next attempt is due.
constexpr uint32_t kBackupWakeLeadMs = 2000;

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
