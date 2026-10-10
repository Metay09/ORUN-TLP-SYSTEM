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
  assert(fake_analog_reads == battery_config::kSamplesPerReading);
  assert(monitor.hasReading() && monitor.readings() == 1);
  assert(monitor.raw() == 2920 && monitor.millivolts() == 3700);
  assert(monitor.readAtMs() == 1000);

  // Nothing before the interval.
  fake_analog_value = 3315;
  assert(!monitor.poll(1000 + battery_config::kReadingIntervalMs - 1));
  assert(fake_analog_reads == battery_config::kSamplesPerReading);
  assert(monitor.millivolts() == 3700);

  assert(monitor.poll(1000 + battery_config::kReadingIntervalMs));
  assert(fake_analog_reads == 2u * battery_config::kSamplesPerReading);
  assert(monitor.readings() == 2 && monitor.millivolts() == 4200);

  // Across the 32-bit millisecond wrap.
  BatteryMonitor wrapped;
  wrapped.begin(UINT32_MAX - 10);
  assert(!wrapped.poll(battery_config::kReadingIntervalMs - 12));
  assert(wrapped.poll(battery_config::kReadingIntervalMs - 11));
}

}  // namespace

int main() {
  conversion();
  readings();
  puts("Battery voltage conversion and reading cadence checks: PASS");
  return 0;
}
