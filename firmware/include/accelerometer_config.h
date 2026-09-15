#pragma once

#include <stdint.h>

namespace orun_tlp::accelerometer_config {

constexpr uint8_t kI2cAddress = 0x18;
constexpr uint8_t kWhoAmIValue = 0x33;

constexpr uint16_t kProbeSampleRateHz = 10;
constexpr uint32_t kProbeSamplePeriodMs = 1000UL / kProbeSampleRateHz;
constexpr uint32_t kProbeTimeoutMs = 500;

constexpr uint8_t kDetectionMaxAttempts = 3;
constexpr uint32_t kDetectionRetryBackoffMs = 250;

static_assert(1000UL % kProbeSampleRateHz == 0,
              "accelerometer sample period must be integral milliseconds");

}  // namespace orun_tlp::accelerometer_config
