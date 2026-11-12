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
  // Meshtastic's LiPo rest-voltage table (4190 = 100 %, 4050 = 90 %; the
  // nRF52 variant with 3.40 V as empty). Owner 2026-11-12: a FULL battery
  // reads 100 % -- that comes from the power state (power_state.h), not from
  // this curve. Bench 2026-11-12 (Hayvan 2, 15 h off USB): with 4.05 V
  // stretched to 100 % the cell sat at "100 %" from 4.12 V down to 4.06 V and
  // its first ~10 % of use never showed. The app uses the same table
  // (edge/app/src/app/herd.ts batteryPercent).
  struct Point {
    uint16_t mv;
    uint8_t percent;
  };
  static const Point kCurve[] = {
      {3400, 0},  {3440, 10}, {3480, 20}, {3530, 30}, {3630, 40}, {3720, 50},
      {3800, 60}, {3890, 70}, {3990, 80}, {4050, 90}, {4190, 100}};
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

bool BatteryLogWindow::setPeriod(uint32_t readings) {
  if (readings > battery_log::kMaxPeriodReadings) return false;
  period_ = static_cast<uint16_t>(readings);
  count_ = 0;
  complete_ = false;
  return true;
}

bool BatteryLogWindow::add(uint32_t millivolts) {
  if (period_ == 0) return false;
  if (count_ == 0 || complete_) {
    count_ = 0;
    complete_ = false;
    lowest_ = millivolts;
    highest_ = millivolts;
  }
  last_ = millivolts;
  if (millivolts < lowest_) lowest_ = millivolts;
  if (millivolts > highest_) highest_ = millivolts;
  if (++count_ < period_) return false;
  complete_ = true;
  return true;
}

}  // namespace orun_tlp
