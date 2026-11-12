#include "battery_monitor.h"

#include <Arduino.h>

#include "monotonic_time.h"

namespace orun_tlp {

uint32_t batteryMillivoltsFromRaw(uint32_t raw) {
  using namespace battery_config;
  const uint64_t numerator = static_cast<uint64_t>(raw) * kFullScaleMv * kDividerX1000;
  const uint64_t denominator = static_cast<uint64_t>(kAdcCounts) * 1000U;
  return static_cast<uint32_t>((numerator + denominator / 2) / denominator);
}

uint32_t BatteryMonitor::scaled(uint32_t raw) const {
  const uint64_t mv = batteryMillivoltsFromRaw(raw);
  return static_cast<uint32_t>((mv * scale_ppm_ + battery_config::kScaleUnit / 2) /
                               battery_config::kScaleUnit);
}

void BatteryMonitor::begin(uint32_t now_ms) {
  // The analog configuration is global; nothing else in this firmware uses
  // the ADC, so it is set once here.
  analogReference(AR_INTERNAL_3_0);
  analogReadResolution(12);
  analogSampleTime(battery_config::kSampleTimeUs);
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

void BatteryMonitor::setReference(uint32_t read_mv, uint32_t reference_mv) {
  using namespace battery_config;
  if (read_mv == 0) return;
  // The scale that would have read `reference_mv`, then halfway there.
  const uint64_t target = static_cast<uint64_t>(scale_ppm_) * reference_mv / read_mv;
  uint64_t next = (static_cast<uint64_t>(scale_ppm_) + target) / 2;
  if (next < kScaleMin) next = kScaleMin;
  if (next > kScaleMax) next = kScaleMax;
  scale_ppm_ = static_cast<uint32_t>(next);
  scale_learned_ = true;
  if (readings_ != 0) millivolts_ = scaled(raw_);
}

bool BatteryMonitor::poll(uint32_t now_ms, bool quiet, bool usb) {
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
  // A few tens of microseconds each; averaging smooths ADC noise, not load sag.
  uint32_t sum = 0;
  for (uint8_t i = 0; i < battery_config::kSamplesPerReading; ++i)
    sum += static_cast<uint32_t>(analogRead(WB_A0));
  raw_ = (sum + battery_config::kSamplesPerReading / 2) / battery_config::kSamplesPerReading;
  millivolts_ = scaled(raw_);
  read_at_ms_ = now_ms;
  last_quiet_ = quiet;
  ++readings_;
  if (!quiet) ++loaded_readings_;
}

}  // namespace orun_tlp
