#pragma once

#include <stdint.h>

namespace orun_tlp {

// Battery voltage on the reference platform: RAK4631 on a WisBlock base
// (RAK19007), where the battery reaches the nRF52840 through a resistor
// divider on WB_A0 (AIN3). Read with the 3.0 V internal reference at 12 bits
// and RAK's published divider compensation. That compensation is a reference
// value, not a calibration of this board: resistor tolerance moves it per
// base board, so the reading is reported as uncalibrated until it has been
// checked against a multimeter.
//
// Measurement only. No low-battery policy acts on it yet.
namespace battery_config {
constexpr uint32_t kFullScaleMv = 3000;       // AR_INTERNAL_3_0
constexpr uint32_t kAdcCounts = 4096;         // 12-bit
constexpr uint32_t kDividerCompX1000 = 1730;  // RAK reference value
constexpr uint8_t kSamplesPerReading = 8;
constexpr uint32_t kReadingIntervalMs = 60000;
}  // namespace battery_config

// Average raw count -> millivolts at the battery, rounded.
uint32_t batteryMillivoltsFromRaw(uint32_t raw);

class BatteryMonitor {
 public:
  // Takes the first reading at once.
  void begin(uint32_t now_ms);
  // Takes a reading when the interval has passed. Returns true when it did.
  bool poll(uint32_t now_ms);

  bool hasReading() const { return readings_ != 0; }
  uint32_t millivolts() const { return millivolts_; }
  uint32_t raw() const { return raw_; }
  uint32_t readAtMs() const { return read_at_ms_; }
  uint32_t readings() const { return readings_; }

 private:
  void read(uint32_t now_ms);

  uint32_t millivolts_ = 0;
  uint32_t raw_ = 0;
  uint32_t read_at_ms_ = 0;
  uint32_t readings_ = 0;
};

}  // namespace orun_tlp
