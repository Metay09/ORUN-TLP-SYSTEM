#pragma once

#include <stdint.h>

namespace orun_tlp {
namespace accelerometer_config {

constexpr uint8_t kI2cAddress = 0x18;
constexpr uint8_t kWhoAmIValue = 0x33;

constexpr uint16_t kProbeSampleRateHz = 10;
constexpr uint32_t kProbeSamplePeriodMs = 1000UL / kProbeSampleRateHz;
// LIS3DH high-resolution mode requires 7/ODR turn-on time. Keep this separate
// from the normal data-ready timeout so an early retained/transition sample
// cannot be accepted merely because ZYXDA is already asserted.
constexpr uint8_t kHighResolutionTurnOnPeriods = 7;
constexpr uint32_t kHighResolutionSettleMs =
    kProbeSamplePeriodMs * kHighResolutionTurnOnPeriods;
constexpr uint32_t kProbeTimeoutMs = 500;

constexpr uint8_t kDetectionMaxAttempts = 3;
constexpr uint32_t kDetectionRetryBackoffMs = 250;
constexpr uint8_t kPowerDownMaxAttempts = 3;
// Once the immediate shutdown budget is exhausted, keep the capability faulted
// but retry one cleanup write only once per minute. This avoids abandoning a
// positively identified sensor in 10 Hz mode while also avoiding a tight shared-
// I2C recovery loop that could interfere with GNSS.
constexpr uint32_t kFaultCleanupRetryBackoffMs = 60UL * 1000UL;

static_assert(1000UL % kProbeSampleRateHz == 0,
              "accelerometer sample period must be integral milliseconds");
static_assert(kHighResolutionSettleMs >= 7UL * kProbeSamplePeriodMs,
              "high-resolution settling must cover LIS3DH 7/ODR turn-on");
static_assert(kPowerDownMaxAttempts > 0,
              "accelerometer power-down must be attempted at least once");

}  // namespace accelerometer_config
}  // namespace orun_tlp
