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

}  // namespace

int main() {
  firstReadingSetsTheState();
  changesNeedConsecutiveConfirmation();
  hysteresis();
  intervalRule();
  puts("Battery state thresholds, confirmation and interval rule checks: PASS");
  return 0;
}
