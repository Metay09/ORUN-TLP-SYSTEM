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

// The tracking interval actually used, given the battery state. Saturates
// instead of overflowing; 0 stays 0.
uint32_t batteryAdjustedIntervalMs(uint32_t interval_ms, BatteryState state);

}  // namespace orun_tlp
