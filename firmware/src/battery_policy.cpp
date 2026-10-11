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

uint8_t batteryPercentEstimate(uint32_t mv) {
  // Typical 1S LiPo open-circuit curve, millivolts at each 5 % step.
  struct Point {
    uint16_t mv;
    uint8_t percent;
  };
  static const Point kCurve[] = {
      {3270, 0},  {3610, 5},  {3690, 10}, {3710, 15}, {3730, 20},
      {3750, 25}, {3770, 30}, {3790, 35}, {3800, 40}, {3820, 45},
      {3840, 50}, {3850, 55}, {3870, 60}, {3910, 65}, {3950, 70},
      {3980, 75}, {4020, 80}, {4080, 85}, {4110, 90}, {4150, 95},
      {4200, 100}};
  constexpr unsigned kPoints = sizeof(kCurve) / sizeof(kCurve[0]);
  if (mv <= kCurve[0].mv) return 0;
  if (mv >= kCurve[kPoints - 1].mv) return 100;
  for (unsigned i = 1; i < kPoints; ++i) {
    if (mv > kCurve[i].mv) continue;
    const Point& lo = kCurve[i - 1];
    const Point& hi = kCurve[i];
    const uint32_t span_mv = hi.mv - lo.mv;
    const uint32_t span_pct = hi.percent - lo.percent;
    return static_cast<uint8_t>(
        lo.percent + ((mv - lo.mv) * span_pct + span_mv / 2) / span_mv);
  }
  return 100;
}

uint32_t batteryAdjustedIntervalMs(uint32_t interval_ms, BatteryState state) {
  if (state != BatteryState::kCritical) return interval_ms;
  constexpr uint32_t kMultiplier = battery_policy::kCriticalIntervalMultiplier;
  if (interval_ms > UINT32_MAX / kMultiplier) return UINT32_MAX;
  return interval_ms * kMultiplier;
}

}  // namespace orun_tlp
