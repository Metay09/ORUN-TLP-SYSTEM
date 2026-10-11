#include "battery_monitor.h"

#include <Arduino.h>

#include "monotonic_time.h"

namespace orun_tlp {

uint32_t batteryMillivoltsFromRaw(uint32_t raw) {
  using namespace battery_config;
  const uint64_t numerator =
      static_cast<uint64_t>(raw) * kFullScaleMv * kDividerCompX1000;
  const uint64_t denominator = static_cast<uint64_t>(kAdcCounts) * 1000U;
  return static_cast<uint32_t>((numerator + denominator / 2) / denominator);
}

void BatteryMonitor::begin(uint32_t now_ms) {
  // The analog configuration is global; nothing else in this firmware uses
  // the ADC, so it is set once here.
  analogReference(AR_INTERNAL_3_0);
  analogReadResolution(12);
  read(now_ms);
}

bool BatteryMonitor::poll(uint32_t now_ms) {
  if (!monotonic::elapsed(now_ms, read_at_ms_,
                          battery_config::kReadingIntervalMs))
    return false;
  read(now_ms);
  return true;
}

void BatteryMonitor::read(uint32_t now_ms) {
  // A few microseconds each; averaging smooths ADC noise, not load sag.
  uint32_t sum = 0;
  for (uint8_t i = 0; i < battery_config::kSamplesPerReading; ++i)
    sum += static_cast<uint32_t>(analogRead(WB_A0));
  raw_ = (sum + battery_config::kSamplesPerReading / 2) /
         battery_config::kSamplesPerReading;
  millivolts_ = batteryMillivoltsFromRaw(raw_);
  read_at_ms_ = now_ms;
  ++readings_;
}

}  // namespace orun_tlp
