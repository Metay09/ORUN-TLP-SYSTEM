#pragma once

#include <stdint.h>

namespace orun_tlp::relay_config {

constexpr uint32_t kMinimumDelayMs = 1200;
constexpr uint32_t kMaximumDelayMs = 4200;
constexpr uint8_t kForwardQueueSize = 4;
constexpr uint8_t kRelayDedupeSize = 16;
constexpr uint8_t kBaseDedupeSize = 32;

static_assert(kMinimumDelayMs <= kMaximumDelayMs, "invalid relay delay range");
static_assert(kMaximumDelayMs - kMinimumDelayMs < 0x80000000UL,
              "relay deadline must fit monotonic half-range");

}  // namespace orun_tlp::relay_config
