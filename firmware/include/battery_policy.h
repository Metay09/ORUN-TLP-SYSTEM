#pragma once

#include <stdint.h>

namespace orun_tlp {

// Battery state from the measured voltage (battery_monitor.h).
// Values match battery_state in ORUN_TLP_V2_PRODUCT_SECURE_WIRE.md 7.4.
enum class BatteryState : uint8_t {
  kUnknown = 0,
  kNormal = 1,
  kLow = 2,
  kCritical = 3,
};

const char* batteryStateName(BatteryState state);

// Owner decisions (2026-10-10), one 1S LiPo:
//   LOW below 3.50 V, back to NORMAL from 3.60 V      -> status only;
//   CRITICAL below 3.35 V, back to LOW from 3.50 V    -> tracking continues
//   at 4x the effective interval (runtime only; the stored interval B and the
//   geofence rule are unchanged, the multiplier applies on top of them).
// A change needs the same new state on kConfirmReadings consecutive readings
// (one a minute), so one reading taken under GNSS/LoRa load does not switch.
namespace battery_policy {
constexpr uint32_t kLowEnterMv = 3500;
constexpr uint32_t kLowExitMv = 3600;
constexpr uint32_t kCriticalEnterMv = 3350;
constexpr uint32_t kCriticalExitMv = 3500;
constexpr uint8_t kConfirmReadings = 3;
constexpr uint32_t kCriticalIntervalMultiplier = 4;
}  // namespace battery_policy

class BatteryStateTracker {
 public:
  // Feed one reading. The first reading sets the state directly; later
  // changes need confirmation. Returns true when the state changed.
  bool update(uint32_t millivolts);
  BatteryState state() const { return state_; }

 private:
  BatteryState state_ = BatteryState::kUnknown;
  BatteryState candidate_ = BatteryState::kUnknown;
  uint8_t candidate_count_ = 0;
};

// Estimated charge in percent from the measured voltage, using a typical
// 1S LiPo rest-voltage curve (linear between points). An estimate only:
// load, temperature, cell age and charging (USB/solar raise the voltage)
// all move it. kBatteryPercentUnknown when there is no reading.
constexpr uint8_t kBatteryPercentUnknown = 255;
uint8_t batteryPercentEstimate(uint32_t millivolts);

// The tracking interval actually used, given the battery state. Saturates
// instead of overflowing; 0 stays 0.
uint32_t batteryAdjustedIntervalMs(uint32_t interval_ms, BatteryState state);

// Periodic battery log line (owner request 2026-10-11): a record of charge
// and discharge (for example with a solar panel), and a coarse average
// consumption from the voltage drop over several days with the panel
// covered. Readings come once a minute (battery_config::kReadingIntervalMs);
// one line per period of that many readings carries the last, lowest and
// highest reading of the period. Period 0 turns the line off.
namespace battery_log {
constexpr uint16_t kDefaultPeriodReadings = 10;
constexpr uint16_t kMaxPeriodReadings = 1440;  // one day
}  // namespace battery_log

class BatteryLogWindow {
 public:
  // Starts a new period. A value above kMaxPeriodReadings is refused and
  // changes nothing.
  bool setPeriod(uint32_t readings);
  uint16_t period() const { return period_; }

  // Folds one reading in. Returns true when it completes a period; last(),
  // lowest(), highest() and count() then describe that period until the
  // next add().
  bool add(uint32_t millivolts);
  uint32_t last() const { return last_; }
  uint32_t lowest() const { return lowest_; }
  uint32_t highest() const { return highest_; }
  uint16_t count() const { return count_; }

 private:
  uint16_t period_ = battery_log::kDefaultPeriodReadings;
  uint16_t count_ = 0;
  bool complete_ = false;
  uint32_t last_ = 0;
  uint32_t lowest_ = 0;
  uint32_t highest_ = 0;
};

}  // namespace orun_tlp
