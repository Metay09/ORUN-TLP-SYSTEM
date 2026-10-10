#include "battery_policy.h"

namespace orun_tlp {
namespace {

// Where the voltage puts the battery, given where it is now (hysteresis).
BatteryState classify(uint32_t mv, BatteryState current) {
  using namespace battery_policy;
  switch (current) {
    case BatteryState::kCritical:
      if (mv >= kLowExitMv) return BatteryState::kNormal;
      if (mv >= kCriticalExitMv) return BatteryState::kLow;
      return BatteryState::kCritical;
    case BatteryState::kLow:
      if (mv < kCriticalEnterMv) return BatteryState::kCritical;
      if (mv >= kLowExitMv) return BatteryState::kNormal;
      return BatteryState::kLow;
    case BatteryState::kNormal:
    case BatteryState::kUnknown:
      break;
  }
  if (mv < kCriticalEnterMv) return BatteryState::kCritical;
  if (mv < kLowEnterMv) return BatteryState::kLow;
  return BatteryState::kNormal;
}

}  // namespace

const char* batteryStateName(BatteryState state) {
  switch (state) {
    case BatteryState::kUnknown: return "UNKNOWN";
    case BatteryState::kNormal: return "NORMAL";
    case BatteryState::kLow: return "LOW";
    case BatteryState::kCritical: return "CRITICAL";
  }
  return "UNKNOWN";
}

bool BatteryStateTracker::update(uint32_t millivolts) {
  const BatteryState next = classify(millivolts, state_);
  if (state_ == BatteryState::kUnknown) {
    state_ = next;
    candidate_count_ = 0;
    return true;
  }
  if (next == state_) {
    candidate_count_ = 0;
    return false;
  }
  if (next != candidate_) {
    candidate_ = next;
    candidate_count_ = 0;
  }
  if (++candidate_count_ < battery_policy::kConfirmReadings) return false;
  state_ = next;
  candidate_count_ = 0;
  return true;
}

uint32_t batteryAdjustedIntervalMs(uint32_t interval_ms, BatteryState state) {
  if (state != BatteryState::kCritical) return interval_ms;
  constexpr uint32_t kMultiplier = battery_policy::kCriticalIntervalMultiplier;
  if (interval_ms > UINT32_MAX / kMultiplier) return UINT32_MAX;
  return interval_ms * kMultiplier;
}

}  // namespace orun_tlp
