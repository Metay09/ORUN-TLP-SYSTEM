#include "monotonic_time.h"

#include <Arduino.h>

namespace orun_tlp::monotonic {

uint32_t nowMs() {
  static_assert(sizeof(TickType_t) == sizeof(uint32_t), "32-bit RTOS ticks required");
  static TickMillis clock;
  return clock.update(xTaskGetTickCount(), configTICK_RATE_HZ);
}

}  // namespace orun_tlp::monotonic
