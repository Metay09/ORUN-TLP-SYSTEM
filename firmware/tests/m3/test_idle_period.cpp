// Y3: how long the loop may sleep between passes (power_manager.h).
#include <assert.h>
#include <stdio.h>

#include "Arduino.h"
#include "power_manager.h"

using namespace orun_tlp;

int main() {
  IdleInputs quiet;
  assert(idlePeriodMs(quiet) == power_config::kQuietIdlePeriodMs);
  // Any one reason keeps the 10 ms service bound.
  IdleInputs in;
  in.usb_power = true;
  assert(idlePeriodMs(in) == power_config::kIdleServicePeriodMs);
  in = IdleInputs(); in.radio_awake = true;
  assert(idlePeriodMs(in) == power_config::kIdleServicePeriodMs);
  in = IdleInputs(); in.gnss_active = true;
  assert(idlePeriodMs(in) == power_config::kIdleServicePeriodMs);
  in = IdleInputs(); in.ble_open = true;
  assert(idlePeriodMs(in) == power_config::kIdleServicePeriodMs);
  in = IdleInputs(); in.storage_busy = true;
  assert(idlePeriodMs(in) == power_config::kIdleServicePeriodMs);
  // Stays under the oversleep alarm (500 ms) and far under the 2 s FIFO drain.
  assert(power_config::kQuietIdlePeriodMs < 500);
  puts("Idle period selection: PASS");
  return 0;
}
