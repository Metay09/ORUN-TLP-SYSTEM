// Pil 2: power state from the quiet battery readings (power_state.h).
#include <assert.h>
#include <stdio.h>

#include "power_state.h"

using namespace orun_tlp;

namespace {

constexpr uint32_t kMin = 60000;

// Bench 2026-11-12, Hayvan 2 (40 us readings, 1.667), scaled to 1.690.
uint32_t bench(uint32_t mv) { return (mv * 1690 + 833) / 1667; }

void benchCharge() {
  PowerStateTracker t;
  uint32_t now = 0;
  // Two days on USB, the charger idle: steady in its float band -> FULL.
  for (int i = 0; i < 20; ++i, now += kMin) t.update(now, bench(4041), true);
  assert(t.state() == PowerState::kFull);
  // Unplugged ten minutes: on battery.
  for (int i = 0; i < 10; ++i, now += kMin) t.update(now, bench(4034), false);
  assert(t.state() == PowerState::kBattery);
  // Plugged again: the charger tops the cell up, then ends its cycle.
  const uint32_t climb[] = {4099, 4104, 4111, 4116, 4119, 4118, 4123, 4127, 4129, 4133,
                            4132, 4135, 4134, 4134, 4138, 4140, 4135, 4141, 4145};
  for (uint32_t mv : climb) {
    t.update(now, bench(mv), true);
    now += kMin;
    assert(t.state() == PowerState::kCharging);
  }
  uint32_t peak = 0;
  assert(!t.takeChargeEndPeak(peak));
  t.update(now, bench(4121), true);   // the drop: charge ended
  now += kMin;
  assert(t.state() == PowerState::kFull);
  assert(t.takeChargeEndPeak(peak) && peak == bench(4145));
  assert(!t.takeChargeEndPeak(peak));   // taken once
  assert(t.chargeEnds() == 1);
  const uint32_t after[] = {4113, 4108, 4104, 4105, 4100, 4104, 4101, 4102, 4099, 4097};
  for (uint32_t mv : after) {
    t.update(now, bench(mv), true);
    now += kMin;
    assert(t.state() == PowerState::kFull);
  }
  // The charger's own top-up later (falls to its restart, climbs): still full.
  for (uint32_t mv = bench(4030); mv < bench(4120); mv += 6, now += kMin)
    t.update(now, mv, true);
  assert(t.state() == PowerState::kFull);
}

void lowBatteryOnUsbCharges() {
  PowerStateTracker t;
  uint32_t now = 0;
  for (uint32_t mv = 3700; mv < 3760; mv += 4, now += kMin) t.update(now, mv, true);
  assert(t.state() == PowerState::kCharging);
  // A flat stretch below the float band is still charging (constant current).
  for (int i = 0; i < 20; ++i, now += kMin) t.update(now, 3900, true);
  assert(t.state() == PowerState::kCharging);
}

void noBattery() {
  PowerStateTracker t;
  uint32_t now = 0;
  // Bench: the battery-less board reads the charger's open output.
  t.update(now, bench(4255), true);
  assert(t.state() == PowerState::kUnknown);   // could be a cell at 4.2 V
  for (int i = 0; i < 119; ++i) { now += kMin; t.update(now, bench(4255), true); }
  assert(t.state() == PowerState::kUnknown);
  now += kMin;
  t.update(now, bench(4262), true);
  assert(t.state() == PowerState::kNoBattery);
  // No calibration from a battery-less board.
  uint32_t peak = 0;
  assert(!t.takeChargeEndPeak(peak));
}

void solarDay() {
  PowerStateTracker t;
  uint32_t now = 0;
  // Night: slowly down, no USB.
  for (uint32_t mv = 3950; mv > 3900; mv -= 2, now += kMin) t.update(now, mv, false);
  assert(t.state() == PowerState::kBattery);
  // Morning sun: climbing.
  uint32_t mv = 3900;
  for (int i = 0; i < 30; ++i, now += kMin, mv += 5) t.update(now, mv, false);
  assert(t.state() == PowerState::kCharging);
  for (; mv < 4190; now += kMin, mv += 3) t.update(now, mv, false);
  assert(t.state() == PowerState::kCharging);
  // The charger ends its cycle in the afternoon.
  t.update(now, mv - 25, false);
  now += kMin;
  assert(t.state() == PowerState::kFull);
  // Evening: down below the restart threshold -> on battery again.
  for (uint32_t v = mv - 30; v > 4000; v -= 3, now += kMin) t.update(now, v, false);
  assert(t.state() == PowerState::kBattery);
}

void noiseIsNotAChargeEnd() {
  PowerStateTracker t;
  uint32_t now = 0;
  // On battery, steady near full: a 20 mV dip is not a charge end.
  for (int i = 0; i < 15; ++i, now += kMin) t.update(now, 4120, false);
  t.update(now, 4100, false);
  uint32_t peak = 0;
  assert(!t.takeChargeEndPeak(peak));
  assert(t.state() == PowerState::kBattery);
}

void calibrationStepIsNotCharging() {
  // Bench 2026-11-12, Hayvan 2 on battery: 14:09 4067, then the hourly
  // offset calibration, 14:14 4087 and level from there.
  PowerStateTracker without, with;
  uint32_t now = 0;
  for (int i = 0; i < 15; ++i, now += kMin) {
    without.update(now, 4068, false);
    with.update(now, 4068, false);
  }
  with.rebase();   // what the firmware does after a calibration
  bool charging_without = false, charging_with = false;
  for (int i = 0; i < 15; ++i, now += kMin) {
    without.update(now, 4087, false);
    with.update(now, 4087, false);
    charging_without |= without.state() == PowerState::kCharging;
    charging_with |= with.state() == PowerState::kCharging;
  }
  assert(charging_without);   // the old false "Şarj oluyor"
  assert(!charging_with && with.state() == PowerState::kBattery);
  // A step down after a calibration is not a charge end either.
  with.rebase();
  with.update(now, 4060, false);
  uint32_t peak = 0;
  assert(!with.takeChargeEndPeak(peak));

  // FULL on USB survives a calibration.
  PowerStateTracker usb;
  now = 0;
  for (int i = 0; i < 20; ++i, now += kMin) usb.update(now, 4110, true);
  assert(usb.state() == PowerState::kFull);
  usb.rebase();
  for (int i = 0; i < 5; ++i, now += kMin) usb.update(now, 4128, true);
  assert(usb.state() == PowerState::kFull);
  // Real sun after a calibration is still seen: a sustained climb.
  PowerStateTracker sun;
  now = 0;
  for (int i = 0; i < 15; ++i, now += kMin) sun.update(now, 3900, false);
  sun.rebase();
  uint32_t mv = 3900;
  for (int i = 0; i < 20; ++i, now += kMin, mv += 4) sun.update(now, mv, false);
  assert(sun.state() == PowerState::kCharging);
}

}  // namespace

int main() {
  assert(powerStateName(PowerState::kNoBattery)[0] == 'N');
  benchCharge();
  lowBatteryOnUsbCharges();
  noBattery();
  solarDay();
  noiseIsNotAChargeEnd();
  calibrationStepIsNotCharging();
  puts("Power state from quiet battery readings: PASS");
  return 0;
}
