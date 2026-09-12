#pragma once

#include <stdint.h>

namespace orun_tlp::gnss_config {

// RAK12500 official I2C example powers the sensor slot via WB_IO2, waits one
// second after each level change, and uses the default I2C address in begin().
constexpr uint32_t kPowerSettleMs = 1000;
constexpr uint8_t kI2cAddress = 0x42;
constexpr uint16_t kConfigurationMaxWaitMs = 250;
constexpr uint32_t kPositionTestIntervalMs = 60000;

}  // namespace orun_tlp::gnss_config
