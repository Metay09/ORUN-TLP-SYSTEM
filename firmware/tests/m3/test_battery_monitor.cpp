// Battery voltage reading on WB_A0 (battery_monitor.h).
#include <assert.h>
#include <stdio.h>

#include "Arduino.h"
#include "battery_monitor.h"

using namespace orun_tlp;

namespace {

void conversion() {
  assert(batteryMillivoltsFromRaw(0) == 0);
  // 3000 mV / 4096 counts * 1.73 = 1.26709 mV per count at the battery.
  assert(batteryMillivoltsFromRaw(2920) == 3700);
  assert(batteryMillivoltsFromRaw(3315) == 4200);
  assert(batteryMillivoltsFromRaw(2688) == 3406);
  assert(batteryMillivoltsFromRaw(4095) == 5189);  // full scale, no overflow
  assert(batteryMillivoltsFromRaw(UINT32_MAX) > 5189);
}

void readings() {
  BatteryMonitor monitor;
  assert(!monitor.hasReading());
  fake_analog_value = 2920;
  fake_analog_reads = 0;
  monitor.begin(1000);
  // Configured as the RAK reference method, first reading taken at once.
  assert(fake_analog_reference == AR_INTERNAL_3_0);
  assert(fake_analog_resolution == 12);
  assert(fake_analog_last_pin == WB_A0);
  // Each reading: 8 samples at 3 us, then 8 at 40 us; offset calibrated.
  assert(fake_analog_reads == 2u * battery_config::kSamplesPerReading);
  assert(fake_analog_sample_time_us == 3);
  assert(fake_analog_calibrations == 1 && monitor.calibrations() == 1);
  assert(monitor.hasReading() && monitor.readings() == 1);
  assert(monitor.raw() == 2920 && monitor.millivolts() == 3700);
  assert(monitor.readAtMs() == 1000);

  // Nothing before the interval.
  fake_analog_value = 3315;
  assert(!monitor.poll(1000 + battery_config::kReadingIntervalMs - 1));
  assert(fake_analog_reads == 2u * battery_config::kSamplesPerReading);
  assert(monitor.millivolts() == 3700);

  assert(monitor.poll(1000 + battery_config::kReadingIntervalMs));
  assert(fake_analog_reads == 4u * battery_config::kSamplesPerReading);
  assert(monitor.readings() == 2 && monitor.millivolts() == 4200);

  // Across the 32-bit millisecond wrap.
  BatteryMonitor wrapped;
  wrapped.begin(UINT32_MAX - 10);
  assert(!wrapped.poll(battery_config::kReadingIntervalMs - 12));
  assert(wrapped.poll(battery_config::kReadingIntervalMs - 11));
}

void slowAndQuiet() {
  using namespace battery_config;
  // 40 us samples use the divider's own ratio 2.5/1.5.
  assert(batteryMillivoltsFromSlowRaw(3360) == 4102);
  BatteryMonitor m;
  fake_analog_value = 3315;       // 4200 mV by the old method
  fake_analog_slow_value = 3440;  // the same input, sampled long enough
  const unsigned calibrations = fake_analog_calibrations;
  m.begin(0);
  assert(m.raw() == 3315 && m.slowRaw() == 3440 && m.millivolts() == 4200);
  assert(m.slowMillivolts() == batteryMillivoltsFromSlowRaw(3440));
  assert(fake_analog_calibrations == calibrations + 1);

  // Due, but the radio is sending: wait for a quiet second.
  uint32_t t = kReadingIntervalMs;
  assert(!m.poll(t, false));
  assert(!m.poll(t + 500, true));                 // quiet only 0 ms so far
  assert(!m.poll(t + 500 + kQuietSettleMs - 1, true));
  assert(m.poll(t + 500 + kQuietSettleMs, true));
  assert(m.lastQuiet() && m.loadedReadings() == 0 && m.readings() == 2);

  // Never quiet: read anyway after the longest wait, counted as loaded.
  const uint32_t at = m.readAtMs();
  assert(!m.poll(at + kReadingIntervalMs, false));
  assert(!m.poll(at + kReadingIntervalMs + kQuietWaitMaxMs - 1, false));
  assert(m.poll(at + kReadingIntervalMs + kQuietWaitMaxMs, false));
  assert(!m.lastQuiet() && m.loadedReadings() == 1);

  // USB: the highest quiet reading is kept; a new USB session starts over.
  uint32_t now = m.readAtMs() + kReadingIntervalMs;
  m.poll(now, true, true);
  fake_analog_value = 3330; fake_analog_slow_value = 3455;
  now += kQuietSettleMs; assert(m.poll(now, true, true));
  assert(m.usbPeakRaw() == 3330 && m.usbPeakSlowRaw() == 3455);
  fake_analog_value = 3300; fake_analog_slow_value = 3420;
  now += kReadingIntervalMs; assert(m.poll(now, true, true));
  assert(m.usbPeakRaw() == 3330);                // highest stays
  now += kReadingIntervalMs; m.poll(now, true, false);   // unplugged
  now += 1; m.poll(now, true, true);                     // plugged again
  assert(m.usbPeakRaw() == 0 && m.usbPeakSlowRaw() == 0);

  // Hourly offset calibration, only in a quiet moment.
  const uint32_t before = m.calibrations();
  now += kCalibrationIntervalMs;
  m.poll(now, false, true);
  assert(m.calibrations() == before);
  m.poll(now + kQuietSettleMs, true, true);
  m.poll(now + 2 * kQuietSettleMs, true, true);
  assert(m.calibrations() == before + 1);
  fake_analog_slow_value = 0;
}

}  // namespace

int main() {
  conversion();
  readings();
  slowAndQuiet();
  puts("Battery voltage conversion and reading cadence checks: PASS");
  return 0;
}
