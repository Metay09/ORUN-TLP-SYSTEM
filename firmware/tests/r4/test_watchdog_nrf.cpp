#include <assert.h>
#include <stdio.h>

#include "Arduino.h"
#include "nrf.h"
#include "watchdog_manager.h"

using orun_tlp::WatchdogManager;

int main() {
  fake_nrf_wdt = {};
  fake_reset_reason = POWER_RESETREAS_DOG_Msk | (1UL << 2);

  WatchdogManager::begin();

  const auto& info = WatchdogManager::bootInfo();
  assert(info.reset_reason == fake_reset_reason);
  assert(info.watchdog_reset);
  assert(NRF_WDT->CONFIG ==
         ((WDT_CONFIG_SLEEP_Run << WDT_CONFIG_SLEEP_Pos) |
          (WDT_CONFIG_HALT_Pause << WDT_CONFIG_HALT_Pos)));
  assert(NRF_WDT->CRV == WatchdogManager::kTimeoutSeconds * 32768UL - 1UL);
  assert(NRF_WDT->RREN == (WDT_RREN_RR0_Enabled << WDT_RREN_RR0_Pos));
  assert(NRF_WDT->TASKS_START == 1);
  assert(NRF_WDT->RR[0] == WDT_RR_RR_Reload);

  puts("R4 nRF watchdog saved reset-reason and exact timeout checks: PASS");
}
