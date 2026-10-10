#pragma once

#include <stdint.h>

inline uint32_t fake_reset_reason = 0;
inline uint32_t readResetReason() { return fake_reset_reason; }

// FreeRTOS surface used by the loop health monitor.
using StackType_t = uint32_t;
inline unsigned fake_stack_high_water_words = 0;
inline unsigned uxTaskGetStackHighWaterMark(void*) {
  return fake_stack_high_water_words;
}
