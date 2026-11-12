// Battery voltage reading on WB_A0 (battery_monitor.h).
#include <assert.h>
#include <stdio.h>

#include "Arduino.h"
#include "battery_monitor.h"

using namespace orun_tlp;

namespace {

void conversion() {
  assert(batteryMillivoltsFromRaw(0) == 0);
  // 3000 mV / 4096 counts * 1.690 = 1.23779 mV per count at the battery.
  assert(batteryMillivoltsFromRaw(3393) == 4200);
  assert(batteryMillivoltsFromRaw(2989) == 3700);
  assert(batteryMillivoltsFromRaw(4095) == 5069);  // full scale, no overflow
  assert(batteryMillivoltsFromRaw(UINT32_MAX) > 5069);
}

void readings() {
  BatteryMonitor monitor;
  assert(!monitor.hasReading());
  fake_analog_value = 2989;
  fake_analog_reads = 0;
  const unsigned calibrations = fake_analog_calibrations;
  monitor.begin(1000);
  // 3.0 V reference, 12 bits, 40 us acquisition (600 kOhm divider), offset
  // calibrated, first reading at once.
  assert(fake_analog_reference == AR_INTERNAL_3_0);
  assert(fake_analog_resolution == 12);
  assert(fake_analog_sample_time_us == 40);
  assert(fake_analog_calibrations == calibrations + 1);
  assert(fake_analog_last_pin == WB_A0);
  assert(fake_analog_reads == battery_config::kSamplesPerReading);
  assert(monitor.hasReading() && monitor.readings() == 1);
  assert(monitor.raw() == 2989 && monitor.millivolts() == 3700);
  assert(monitor.readAtMs() == 1000);

  // Nothing before the interval.
  fake_analog_value = 3393;
  assert(!monitor.poll(1000 + battery_config::kReadingIntervalMs - 1));
  assert(monitor.millivolts() == 3700);
  assert(monitor.poll(1000 + battery_config::kReadingIntervalMs));
  assert(monitor.readings() == 2 && monitor.millivolts() == 4200);

  // Across the 32-bit millisecond wrap.
  BatteryMonitor wrapped;
  wrapped.begin(UINT32_MAX - 10);
  assert(!wrapped.poll(battery_config::kReadingIntervalMs - 12));
  assert(wrapped.poll(battery_config::kReadingIntervalMs - 11));
}

void quietAndCalibration() {
  using namespace battery_config;
  BatteryMonitor m;
  fake_analog_value = 3393;
  m.begin(0);
  // Due, but the radio is sending: wait for a quiet second.
  const uint32_t t = kReadingIntervalMs;
  assert(!m.poll(t, false));
  assert(!m.poll(t + 500, true));
  assert(!m.poll(t + 500 + kQuietSettleMs - 1, true));
  assert(m.poll(t + 500 + kQuietSettleMs, true, true));
  assert(m.lastQuiet() && m.loadedReadings() == 0 && m.usb());

  // Never quiet: read anyway after the longest wait, counted as loaded.
  const uint32_t at = m.readAtMs();
  assert(!m.poll(at + kReadingIntervalMs, false));
  assert(!m.poll(at + kReadingIntervalMs + kQuietWaitMaxMs - 1, false));
  assert(m.poll(at + kReadingIntervalMs + kQuietWaitMaxMs, false));
  assert(!m.lastQuiet() && m.loadedReadings() == 1);

  // Hourly offset calibration, only in a quiet moment.
  const uint32_t before = m.calibrations();
  uint32_t now = m.readAtMs() + kCalibrationIntervalMs;
  m.poll(now, false);
  assert(m.calibrations() == before);
  m.poll(now + kQuietSettleMs, true);
  m.poll(now + 2 * kQuietSettleMs, true);
  assert(m.calibrations() == before + 1);
}

void boardScale() {
  using namespace battery_config;
  BatteryMonitor m;
  fake_analog_value = 3393;   // 4200 at the default scale
  m.begin(0);
  assert(m.scalePpm() == kScaleUnit && !m.scaleLearned());
  // This board read 4150 while the charger held 4200: halfway toward +1.2 %.
  m.setReference(4150, 4200);
  assert(m.scaleLearned());
  assert(m.scalePpm() == 1006024);
  assert(m.millivolts() == 4225);   // the same raw, this board's scale
  // A wild reference is held to +-3 %.
  for (int i = 0; i < 10; ++i) m.setReference(3800, 4200);
  assert(m.scalePpm() == kScaleMax);
  for (int i = 0; i < 10; ++i) m.setReference(4600, 4200);
  assert(m.scalePpm() == kScaleMin);
  m.setReference(0, 4200);   // nothing to learn from
  assert(m.scalePpm() == kScaleMin);
}

}  // namespace

int main() {
  conversion();
  readings();
  quietAndCalibration();
  boardScale();
  puts("Battery voltage conversion, reading cadence and board scale checks: PASS");
  return 0;
}
