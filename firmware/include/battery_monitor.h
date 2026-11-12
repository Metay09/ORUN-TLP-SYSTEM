#pragma once

#include <stdint.h>

namespace orun_tlp {

// Battery voltage on the reference platform: RAK4631 on a WisBlock base
// (RAK19007), where the battery reaches the nRF52840 through a 1 M / 1.5 M
// resistor divider on WB_A0 (AIN3), read with the 3.0 V internal reference at
// 12 bits (claude/ORUN_PIL_OLCUM.md).
//
// - 40 us acquisition: the divider is a 600 kOhm source; Nordic gives 40 us
//   for up to 800 kOhm (the Arduino core default, 3 us, is for 10 kOhm).
// - Scale 1.690: the divider's 2.5/1.5, corrected on the bench (2026-11-12)
//   against the charger's own 4.20 V at the end of a charge (Hayvan 2 read
//   4145 mV with 1.667). Boards differ by a few percent (resistor and
//   reference tolerance), so each board corrects itself the same way: when
//   the power state sees a charge end, setReference() moves this board's
//   scale toward 4200 / reading, within +-3 % and halfway each time. RAM
//   only: after a reboot the next charge end corrects it again.
// - A reading waits for a quiet moment (no radio transmission, no GNSS
//   receiver running, quiet for a second); if none comes it is taken anyway
//   and counted as loaded.
// - SAADC offset calibration at boot and when the chip temperature has moved
//   10 C since the last one (Nordic's rule). Each calibration can shift the
//   reading by 10-20 mV at the battery (bench 2026-11-12: hourly calibration
//   made steps of +6, +9 and +20 mV that read as "charging" and as drain);
//   calibrations() lets the power state start its rise check afresh.
namespace battery_config {
constexpr uint32_t kFullScaleMv = 3000;          // AR_INTERNAL_3_0
constexpr uint32_t kAdcCounts = 4096;            // 12-bit
constexpr uint32_t kDividerX1000 = 1690;         // 2.5/1.5, bench-corrected
constexpr uint8_t kSamplesPerReading = 8;
constexpr uint8_t kSampleTimeUs = 40;            // Nordic: up to 800 kOhm
constexpr uint32_t kReadingIntervalMs = 60000;
constexpr uint32_t kQuietSettleMs = 1000;        // after the load stops
constexpr uint32_t kQuietWaitMaxMs = 240000;     // then read anyway (loaded)
constexpr uint32_t kTemperatureCheckMs = 600000;   // look at the chip every 10 min
constexpr int32_t kRecalibrateDeciC = 100;          // 10 C since the last calibration
constexpr uint32_t kScaleUnit = 1000000;         // board scale, parts per million
constexpr uint32_t kScaleMin = 970000;
constexpr uint32_t kScaleMax = 1030000;
}  // namespace battery_config

// Average raw count -> millivolts at the battery with the default scale.
uint32_t batteryMillivoltsFromRaw(uint32_t raw);

class BatteryMonitor {
 public:
  // Calibrates the offset and takes the first reading at once.
  void begin(uint32_t now_ms);
  // Takes a reading when one is due and the board is quiet (or has not been
  // for too long). `usb` is the USB supply (VBUS) seen by the nRF52840.
  // Returns true when it read.
  bool poll(uint32_t now_ms, bool quiet = true, bool usb = false);

  // The charger held the cell at `reference_mv` while this board read
  // `read_mv` (both in this board's current millivolts): move the scale.
  void setReference(uint32_t read_mv, uint32_t reference_mv);

  bool hasReading() const { return readings_ != 0; }
  uint32_t millivolts() const { return millivolts_; }
  uint32_t raw() const { return raw_; }
  uint32_t scalePpm() const { return scale_ppm_; }
  bool scaleLearned() const { return scale_learned_; }
  uint32_t readAtMs() const { return read_at_ms_; }
  uint32_t readings() const { return readings_; }
  uint32_t loadedReadings() const { return loaded_readings_; }
  uint32_t calibrations() const { return calibrations_; }
  // Chip temperature at the last look, tenths of a degree C.
  int32_t temperatureDeciC() const { return temperature_dc_; }
  bool lastQuiet() const { return last_quiet_; }
  bool usb() const { return usb_; }

 private:
  void read(uint32_t now_ms, bool quiet);
  void calibrate(uint32_t now_ms);
  uint32_t scaled(uint32_t raw) const;

  uint32_t millivolts_ = 0;
  uint32_t raw_ = 0;
  uint32_t scale_ppm_ = battery_config::kScaleUnit;
  bool scale_learned_ = false;
  uint32_t read_at_ms_ = 0;
  uint32_t readings_ = 0;
  uint32_t loaded_readings_ = 0;
  uint32_t calibrations_ = 0;
  int32_t calibrated_dc_ = 0;      // chip temperature at the last calibration
  int32_t temperature_dc_ = 0;
  uint32_t temperature_at_ms_ = 0;
  uint32_t quiet_since_ms_ = 0;
  bool quiet_ = false;
  bool last_quiet_ = false;
  bool usb_ = false;
};

}  // namespace orun_tlp
