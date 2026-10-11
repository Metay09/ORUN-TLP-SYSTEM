// Battery state thresholds, confirmation and interval rule (battery_policy.h).
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "battery_policy.h"
#include "geofence_runtime_policy.h"

using namespace orun_tlp;
using namespace orun_tlp::battery_policy;

namespace {

// Feed the same reading until the state stops changing (bounded).
BatteryState settle(BatteryStateTracker& tracker, uint32_t mv) {
  for (int i = 0; i < 2 * kConfirmReadings; ++i) tracker.update(mv);
  return tracker.state();
}

void firstReadingSetsTheState() {
  struct {
    uint32_t mv;
    BatteryState expected;
  } cases[] = {{4200, BatteryState::kNormal},  {kLowEnterMv, BatteryState::kNormal},
               {kLowEnterMv - 1, BatteryState::kLow},
               {kCriticalEnterMv, BatteryState::kLow},
               {kCriticalEnterMv - 1, BatteryState::kCritical},
               {0, BatteryState::kCritical}};
  for (const auto& c : cases) {
    BatteryStateTracker tracker;
    assert(tracker.state() == BatteryState::kUnknown);
    assert(tracker.update(c.mv));
    assert(tracker.state() == c.expected);
  }
}

void changesNeedConsecutiveConfirmation() {
  BatteryStateTracker tracker;
  tracker.update(4000);
  // Two low readings, then a normal one: the count restarts.
  assert(!tracker.update(3400));
  assert(!tracker.update(3400));
  assert(!tracker.update(4000));
  assert(!tracker.update(3400));
  assert(!tracker.update(3400));
  assert(tracker.state() == BatteryState::kNormal);
  assert(tracker.update(3400));
  assert(tracker.state() == BatteryState::kLow);

  // A different candidate restarts the count as well.
  BatteryStateTracker mixed;
  mixed.update(4000);
  assert(!mixed.update(3400));   // LOW candidate
  assert(!mixed.update(3300));   // CRITICAL candidate, count 1
  assert(!mixed.update(3300));
  assert(mixed.update(3300));
  assert(mixed.state() == BatteryState::kCritical);
}

void hysteresis() {
  BatteryStateTracker tracker;
  tracker.update(4000);
  assert(settle(tracker, kLowEnterMv - 1) == BatteryState::kLow);
  // Between LOW enter and exit: stays LOW.
  assert(settle(tracker, kLowEnterMv) == BatteryState::kLow);
  assert(settle(tracker, kLowExitMv - 1) == BatteryState::kLow);
  assert(settle(tracker, kLowExitMv) == BatteryState::kNormal);

  assert(settle(tracker, kCriticalEnterMv - 1) == BatteryState::kCritical);
  // Between CRITICAL enter and exit: stays CRITICAL.
  assert(settle(tracker, kCriticalEnterMv) == BatteryState::kCritical);
  assert(settle(tracker, kCriticalExitMv - 1) == BatteryState::kCritical);
  assert(settle(tracker, kCriticalExitMv) == BatteryState::kLow);
  assert(settle(tracker, kCriticalEnterMv - 1) == BatteryState::kCritical);
  // Straight back to NORMAL when the charge is high enough.
  assert(settle(tracker, kLowExitMv) == BatteryState::kNormal);
}

void intervalRule() {
  assert(batteryAdjustedIntervalMs(180000, BatteryState::kUnknown) == 180000);
  assert(batteryAdjustedIntervalMs(180000, BatteryState::kNormal) == 180000);
  assert(batteryAdjustedIntervalMs(180000, BatteryState::kLow) == 180000);
  assert(batteryAdjustedIntervalMs(180000, BatteryState::kCritical) == 720000);
  assert(batteryAdjustedIntervalMs(0, BatteryState::kCritical) == 0);
  assert(batteryAdjustedIntervalMs(UINT32_MAX / 2, BatteryState::kCritical) ==
         UINT32_MAX);
  // On top of the geofence rule: OUTSIDE B/3, then x4.
  const uint32_t outside = geofence_runtime_policy::effectiveTrackingIntervalMs(
      180, GeofenceCadenceMode::kBaseDividedBy3);
  assert(outside == 60000);
  assert(batteryAdjustedIntervalMs(outside, BatteryState::kCritical) == 240000);
  // The longest stored interval still fits after x4.
  assert(batteryAdjustedIntervalMs(12UL * 24 * 3600 * 1000,
                                   BatteryState::kCritical) ==
         4UL * 12 * 24 * 3600 * 1000);

  assert(strcmp(batteryStateName(BatteryState::kCritical), "CRITICAL") == 0);
  assert(static_cast<uint8_t>(BatteryState::kLow) == 2);  // wire value 7.4
}

void percentEstimate() {
  assert(batteryPercentEstimate(4200) == 100);
  assert(batteryPercentEstimate(4300) == 100);
  assert(batteryPercentEstimate(3270) == 0);
  assert(batteryPercentEstimate(2900) == 0);
  assert(batteryPercentEstimate(3840) == 50);
  assert(batteryPercentEstimate(4020) == 80);
  assert(batteryPercentEstimate(3995) == 77);  // between 3980/75 and 4020/80
  assert(batteryPercentEstimate(kLowEnterMv) == 3);
  assert(batteryPercentEstimate(kCriticalEnterMv) == 1);
  uint8_t previous = 0;
  for (uint32_t mv = 3000; mv <= 4400; mv += 5) {
    const uint8_t p = batteryPercentEstimate(mv);
    assert(p <= 100 && p >= previous);  // monotonic
    previous = p;
  }
}

void logWindow() {
  using namespace battery_log;
  BatteryLogWindow log;
  assert(log.period() == kDefaultPeriodReadings);
  // One line per period: last, lowest and highest of that period only.
  const uint32_t first[] = {4100, 3900, 4150, 4000, 4000,
                            4000, 4000, 4000, 4000, 3980};
  for (unsigned i = 0; i < 9; ++i) assert(!log.add(first[i]));
  assert(log.add(first[9]));
  assert(log.last() == 3980 && log.lowest() == 3900 &&
         log.highest() == 4150 && log.count() == 10);
  // The next period starts fresh.
  for (unsigned i = 0; i < 9; ++i) assert(!log.add(3700));
  assert(log.add(3750));
  assert(log.last() == 3750 && log.lowest() == 3700 &&
         log.highest() == 3750 && log.count() == 10);

  // A new period length restarts counting; a too-long one changes nothing.
  assert(!log.add(3600));
  assert(log.setPeriod(2));
  assert(!log.add(3500));
  assert(log.add(3550));
  assert(log.lowest() == 3500 && log.count() == 2);
  assert(!log.setPeriod(kMaxPeriodReadings + 1));
  assert(log.period() == 2);
  assert(log.setPeriod(kMaxPeriodReadings));
  assert(log.setPeriod(1));
  assert(log.add(3333) && log.add(3334) && log.lowest() == 3334);

  // 0 turns it off.
  assert(log.setPeriod(0));
  for (int i = 0; i < 3000; ++i) assert(!log.add(3800));
  assert(log.setPeriod(3));
  assert(!log.add(1) && !log.add(2) && log.add(3) && log.lowest() == 1);
}

}  // namespace

int main() {
  firstReadingSetsTheState();
  changesNeedConsecutiveConfirmation();
  hysteresis();
  intervalRule();
  percentEstimate();
  logWindow();
  puts("Battery state thresholds, confirmation and interval rule checks: PASS");
  return 0;
}
