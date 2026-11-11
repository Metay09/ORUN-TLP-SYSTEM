#include "battery_monitor.h"

#include <Arduino.h>

#include "monotonic_time.h"

namespace orun_tlp {

namespace {
uint32_t toMillivolts(uint32_t raw, uint32_t factor_x1000) {
  using namespace battery_config;
  const uint64_t numerator = static_cast<uint64_t>(raw) * kFullScaleMv * factor_x1000;
  const uint64_t denominator = static_cast<uint64_t>(kAdcCounts) * 1000U;
  return static_cast<uint32_t>((numerator + denominator / 2) / denominator);
}

uint32_t averageRead() {
  uint32_t sum = 0;
  for (uint8_t i = 0; i < battery_config::kSamplesPerReading; ++i)
    sum += static_cast<uint32_t>(analogRead(WB_A0));
  return (sum + battery_config::kSamplesPerReading / 2) / battery_config::kSamplesPerReading;
}
}  // namespace

uint32_t batteryMillivoltsFromRaw(uint32_t raw) {
  return toMillivolts(raw, battery_config::kDividerCompX1000);
}

uint32_t batteryMillivoltsFromSlowRaw(uint32_t raw) {
  return toMillivolts(raw, battery_config::kDividerRatioX1000);
}

void BatteryMonitor::begin(uint32_t now_ms) {
  // The analog configuration is global; nothing else in this firmware uses
  // the ADC, so it is set once here.
  analogReference(AR_INTERNAL_3_0);
  analogReadResolution(12);
  quiet_ = true;  // until poll() says otherwise
  quiet_since_ms_ = now_ms;
  calibrate(now_ms);
  read(now_ms, true);
}

void BatteryMonitor::calibrate(uint32_t now_ms) {
  analogCalibrateOffset();
  calibrated_at_ms_ = now_ms;
  ++calibrations_;
}

bool BatteryMonitor::poll(uint32_t now_ms, bool quiet, bool usb) {
  if (usb && !usb_) usb_peak_raw_ = usb_peak_slow_raw_ = 0;  // a new USB session
  usb_ = usb;
  if (!quiet) {
    quiet_ = false;
  } else if (!quiet_) {
    quiet_ = true;
    quiet_since_ms_ = now_ms;
  }
  const bool settled = quiet_ && monotonic::elapsed(now_ms, quiet_since_ms_,
                                                    battery_config::kQuietSettleMs);
  if (settled && monotonic::elapsed(now_ms, calibrated_at_ms_,
                                    battery_config::kCalibrationIntervalMs))
    calibrate(now_ms);
  if (!monotonic::elapsed(now_ms, read_at_ms_, battery_config::kReadingIntervalMs))
    return false;
  if (!settled && !monotonic::elapsed(now_ms, read_at_ms_,
                                      battery_config::kReadingIntervalMs +
                                          battery_config::kQuietWaitMaxMs))
    return false;
  read(now_ms, settled);
  return true;
}

void BatteryMonitor::read(uint32_t now_ms, bool quiet) {
  // A few microseconds each; averaging smooths ADC noise, not load sag.
  raw_ = averageRead();
  analogSampleTime(battery_config::kSlowSampleTimeUs);
  slow_raw_ = averageRead();
  analogSampleTime(3);  // back to the core default (any other value)
  millivolts_ = batteryMillivoltsFromRaw(raw_);
  read_at_ms_ = now_ms;
  last_quiet_ = quiet;
  ++readings_;
  if (!quiet) ++loaded_readings_;
  if (quiet && usb_) {
    if (raw_ > usb_peak_raw_) usb_peak_raw_ = raw_;
    if (slow_raw_ > usb_peak_slow_raw_) usb_peak_slow_raw_ = slow_raw_;
  }
}

}  // namespace orun_tlp
