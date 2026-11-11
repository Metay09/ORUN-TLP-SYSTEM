#pragma once

#include <stdint.h>

namespace orun_tlp {

// Battery voltage on the reference platform: RAK4631 on a WisBlock base
// (RAK19007), where the battery reaches the nRF52840 through a 1 M / 1.5 M
// resistor divider on WB_A0 (AIN3), read with the 3.0 V internal reference at
// 12 bits.
//
// Pil 1 (owner 2026-11-12, claude/ORUN_PIL_OLCUM.md):
// - The divider is a 600 kOhm source. Nordic gives 3 us acquisition for at
//   most 10 kOhm and 40 us for at most 800 kOhm; the Arduino core default is
//   3 us. Every reading now also takes the same samples at 40 us (raw40),
//   converted with the divider's own ratio 2.5/1.5, so the bench shows how far
//   the old method reads low. The reported value stays the old method (3 us,
//   RAK's 1.73) until that is settled; then one method remains.
// - A reading waits for a quiet moment (no radio transmission, no GNSS
//   acquisition, quiet for a second) so it is the battery, not the load sag.
//   If no quiet moment comes it is taken anyway and counted as loaded.
// - SAADC offset calibration at boot and hourly (Nordic: after a 10 C change).
// - With USB power the highest quiet reading is kept: while the charger holds
//   the battery at its 4.20 V (+-1 %) float, that is a reference for the scale
//   without a multimeter.
namespace battery_config {
constexpr uint32_t kFullScaleMv = 3000;          // AR_INTERNAL_3_0
constexpr uint32_t kAdcCounts = 4096;            // 12-bit
constexpr uint32_t kDividerCompX1000 = 1730;     // RAK reference value (3 us)
constexpr uint32_t kDividerRatioX1000 = 1667;    // (1 M + 1.5 M) / 1.5 M
constexpr uint8_t kSamplesPerReading = 8;
constexpr uint8_t kSlowSampleTimeUs = 40;        // Nordic: up to 800 kOhm
constexpr uint32_t kReadingIntervalMs = 60000;
constexpr uint32_t kQuietSettleMs = 1000;        // after the load stops
constexpr uint32_t kQuietWaitMaxMs = 240000;     // then read anyway (loaded)
constexpr uint32_t kCalibrationIntervalMs = 3600000;
}  // namespace battery_config

// Average raw count -> millivolts at the battery, rounded (3 us, 1.73).
uint32_t batteryMillivoltsFromRaw(uint32_t raw);
// Same for a 40 us reading with the divider's own ratio.
uint32_t batteryMillivoltsFromSlowRaw(uint32_t raw);

class BatteryMonitor {
 public:
  // Calibrates the offset and takes the first reading at once.
  void begin(uint32_t now_ms);
  // Takes a reading when one is due and the board is quiet (or has not been
  // for too long). `usb` is the USB supply (VBUS) seen by the nRF52840.
  // Returns true when it read.
  bool poll(uint32_t now_ms, bool quiet = true, bool usb = false);

  bool hasReading() const { return readings_ != 0; }
  uint32_t millivolts() const { return millivolts_; }
  uint32_t raw() const { return raw_; }
  uint32_t slowRaw() const { return slow_raw_; }
  uint32_t slowMillivolts() const { return batteryMillivoltsFromSlowRaw(slow_raw_); }
  uint32_t readAtMs() const { return read_at_ms_; }
  uint32_t readings() const { return readings_; }
  uint32_t loadedReadings() const { return loaded_readings_; }
  uint32_t calibrations() const { return calibrations_; }
  bool lastQuiet() const { return last_quiet_; }
  bool usb() const { return usb_; }
  // Highest quiet reading since USB power came (0 when none).
  uint32_t usbPeakRaw() const { return usb_peak_raw_; }
  uint32_t usbPeakSlowRaw() const { return usb_peak_slow_raw_; }

 private:
  void read(uint32_t now_ms, bool quiet);
  void calibrate(uint32_t now_ms);

  uint32_t millivolts_ = 0;
  uint32_t raw_ = 0;
  uint32_t slow_raw_ = 0;
  uint32_t read_at_ms_ = 0;
  uint32_t readings_ = 0;
  uint32_t loaded_readings_ = 0;
  uint32_t calibrations_ = 0;
  uint32_t calibrated_at_ms_ = 0;
  uint32_t quiet_since_ms_ = 0;
  bool quiet_ = false;
  bool last_quiet_ = false;
  bool usb_ = false;
  uint32_t usb_peak_raw_ = 0;
  uint32_t usb_peak_slow_raw_ = 0;
};

}  // namespace orun_tlp
