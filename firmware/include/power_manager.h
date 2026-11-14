#pragma once

#include <Arduino.h>

namespace orun_tlp {

// What decides how long the loop may sleep between passes (Y3). Every field
// is "something needs the 10 ms service bound right now".
struct IdleInputs {
  bool usb_power = false;        // USB commands; on USB the battery is not spent
  bool radio_awake = false;      // transmitting, or listening (window/continuous)
  bool gnss_active = false;      // receiver powered: detecting, acquiring, fix ready
  bool ble_open = false;         // advertising, connected or closing
  bool storage_busy = false;     // a flash job or a store-before-send is in flight
};

namespace power_config {
constexpr uint32_t kIdleServicePeriodMs = 10;
// Between reports on battery nothing needs servicing faster than this: the
// accelerometer FIFO is drained every 2 s, the battery read once a minute,
// the watchdog fed within 30 s, and the report schedule tolerates a fraction
// of a second. Below LoopHealthMonitor::kIdleOversleepThresholdMs (500).
constexpr uint32_t kQuietIdlePeriodMs = 200;
}  // namespace power_config

constexpr uint32_t idlePeriodMs(const IdleInputs& in) {
  return (in.usb_power || in.radio_awake || in.gnss_active || in.ble_open ||
          in.storage_busy)
             ? power_config::kIdleServicePeriodMs
             : power_config::kQuietIdlePeriodMs;
}

struct PowerManager {
  static void idle(uint32_t period_ms) { delay(period_ms); }
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
