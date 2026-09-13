#include "watchdog_manager.h"

#if defined(NRF52_SERIES)
#include <nrf.h>
#endif

namespace orun_tlp {

WatchdogManager::BootInfo WatchdogManager::boot_info_{};

void WatchdogManager::begin() {
#if defined(NRF52_SERIES)
  const uint32_t reason = NRF_POWER->RESETREAS;
  boot_info_.reset_reason = reason;
  boot_info_.watchdog_reset = (reason & POWER_RESETREAS_DOG_Msk) != 0;
  // RESETREAS bits are cleared by writing one to each set bit.
  NRF_POWER->RESETREAS = reason;

  // WDT cannot be stopped after TASKS_START until reset. Run it during normal
  // sleep so a wedged loop is still recovered; pause while a debugger halts CPU.
  if (NRF_WDT->RUNSTATUS == 0) {
    NRF_WDT->CONFIG =
        (WDT_CONFIG_SLEEP_Run << WDT_CONFIG_SLEEP_Pos) |
        (WDT_CONFIG_HALT_Pause << WDT_CONFIG_HALT_Pos);
    NRF_WDT->CRV = kTimeoutSeconds * 32768UL;
    NRF_WDT->RREN =
        (WDT_RREN_RR0_Enabled << WDT_RREN_RR0_Pos);
    NRF_WDT->TASKS_START = 1;
  }
  feed();
#else
  boot_info_ = {};
#endif
}

void WatchdogManager::feed() {
#if defined(NRF52_SERIES)
  NRF_WDT->RR[0] = WDT_RR_RR_Reload;
#endif
}

const WatchdogManager::BootInfo& WatchdogManager::bootInfo() {
  return boot_info_;
}

}  // namespace orun_tlp
