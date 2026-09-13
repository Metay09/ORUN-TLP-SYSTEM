#pragma once

#include <stdint.h>

struct FakeNrfWdt {
  uint32_t TASKS_START = 0;
  uint32_t RESERVED0[63]{};
  uint32_t RUNSTATUS = 0;
  uint32_t REQSTATUS = 0;
  uint32_t RESERVED1[63]{};
  uint32_t CRV = 0;
  uint32_t RREN = 0;
  uint32_t CONFIG = 0;
  uint32_t RESERVED2[60]{};
  uint32_t RR[8]{};
};

inline FakeNrfWdt fake_nrf_wdt{};
#define NRF_WDT (&fake_nrf_wdt)

#define POWER_RESETREAS_DOG_Pos 1UL
#define POWER_RESETREAS_DOG_Msk (1UL << POWER_RESETREAS_DOG_Pos)

#define WDT_CONFIG_SLEEP_Pos 0UL
#define WDT_CONFIG_SLEEP_Run 1UL
#define WDT_CONFIG_HALT_Pos 3UL
#define WDT_CONFIG_HALT_Pause 0UL

#define WDT_RREN_RR0_Pos 0UL
#define WDT_RREN_RR0_Enabled 1UL

#define WDT_RR_RR_Reload 0x6E524635UL
