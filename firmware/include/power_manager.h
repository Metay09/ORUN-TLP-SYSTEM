#pragma once

#include <Arduino.h>

namespace orun_tlp {

struct PowerManager {
  static void idle() {
    // In Adafruit nRF52 1.7.0 delay flushes USB CDC and blocks the loop task
    // via vTaskDelay. FreeRTOS tickless idle performs the supported event wait
    // and RTC tick correction when no task is runnable. IRQ tasks remain active.
    // Keep a short service bound for GNSS and radio; no SYSTEM OFF or raw WFE.
    constexpr uint32_t kIdleServicePeriodMs = 10;
    delay(kIdleServicePeriodMs);
  }
};

}  // namespace orun_tlp
