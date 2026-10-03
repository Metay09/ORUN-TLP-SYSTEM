#include "monotonic_time.h"

#include <Arduino.h>

namespace orun_tlp::monotonic {
namespace {

static_assert(sizeof(TickType_t) == sizeof(uint32_t),
              "32-bit RTOS ticks required");

TickMillis clock;

uint64_t updateClock() {
  return clock.update64(xTaskGetTickCount(), configTICK_RATE_HZ);
}

}  // namespace

uint32_t nowMs() {
  return static_cast<uint32_t>(updateClock());
}

uint64_t nowMs64() {
  return updateClock();
}

}  // namespace orun_tlp::monotonic
