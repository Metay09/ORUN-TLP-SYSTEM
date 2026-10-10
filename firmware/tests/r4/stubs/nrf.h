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

// --- Loop health monitor fakes (RTC2, POWER.GPREGRET2, NVIC) ---------------
struct FakeNrfRtc {
  uint32_t TASKS_START = 0;
  uint32_t TASKS_STOP = 0;
  uint32_t TASKS_CLEAR = 0;
  uint32_t EVENTS_COMPARE[4]{};
  uint32_t INTENSET = 0;
  uint32_t COUNTER = 0;
  uint32_t PRESCALER = 0;
  uint32_t CC[4]{};
};
inline FakeNrfRtc fake_nrf_rtc2{};
#define NRF_RTC2 (&fake_nrf_rtc2)
#define RTC_INTENSET_COMPARE0_Msk (1UL << 16)

struct FakeNrfPower {
  uint32_t GPREGRET = 0;
  uint32_t GPREGRET2 = 0;
};
inline FakeNrfPower fake_nrf_power{};
#define NRF_POWER (&fake_nrf_power)

constexpr int RTC2_IRQn = 36;
inline int fake_nvic_enabled_irq = -1;
inline int fake_nvic_cleared_irq = -1;
inline int fake_nvic_priority_irq = -1;
inline uint32_t fake_nvic_priority = 0;
inline void NVIC_SetPriority(int irq, uint32_t priority) {
  fake_nvic_priority_irq = irq;
  fake_nvic_priority = priority;
}
inline void NVIC_ClearPendingIRQ(int irq) { fake_nvic_cleared_irq = irq; }
inline void NVIC_EnableIRQ(int irq) { fake_nvic_enabled_irq = irq; }
inline void __NOP() {}
