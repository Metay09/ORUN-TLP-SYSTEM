#include "loop_health_monitor.h"

#include <Arduino.h>

#if defined(NRF52_SERIES)
#include <nrf.h>
#include <nrf_sdm.h>
#include <nrf_soc.h>
#endif

namespace orun_tlp {

volatile uint8_t LoopHealthMonitor::current_stage_ =
    static_cast<uint8_t>(LoopStage::kNone);
volatile uint32_t LoopHealthMonitor::completed_passes_ = 0;
LoopStallRecord LoopHealthMonitor::previous_boot_{};

namespace {

LoopStallDetector stall_detector(LoopHealthMonitor::kCaptureAfterTicks);
IdleSleepStats idle_stats(LoopHealthMonitor::kIdleOversleepThresholdMs);

#if defined(NRF52_SERIES)

// 32768 Hz / (4095 + 1) = 8 Hz: 125 ms per count, 24-bit counter.
constexpr uint32_t kRtcPrescaler = 4095;
constexpr uint32_t kRtcCountsPerSecond = 8;
constexpr uint32_t kRtcCounterMask = 0x00FFFFFFUL;
constexpr uint32_t kTickCounts =
    LoopHealthMonitor::kTickPeriodMs * kRtcCountsPerSecond / 1000UL;
static_assert(kTickCounts >= 2, "RTC compare needs at least two counts");

// Application interrupt priority that may still call the SoftDevice.
constexpr uint32_t kTimerInterruptPriority = 6;
// GPREGRET2. GPREGRET (id 0) belongs to the bootloader's DFU handshake.
constexpr uint32_t kRetainedRegisterId = 1;

// POWER is a SoftDevice-restricted peripheral: go through the SoC API while
// the SoftDevice is enabled, directly otherwise. Task or interrupt context.
uint8_t readRetained() {
  uint8_t softdevice_enabled = 0;
  (void)sd_softdevice_is_enabled(&softdevice_enabled);
  if (softdevice_enabled != 0) {
    uint32_t value = 0;
    if (sd_power_gpregret_get(kRetainedRegisterId, &value) != NRF_SUCCESS)
      return loop_stall_record::kEmpty;
    return static_cast<uint8_t>(value);
  }
  return static_cast<uint8_t>(NRF_POWER->GPREGRET2);
}

void writeRetained(uint8_t value) {
  uint8_t softdevice_enabled = 0;
  (void)sd_softdevice_is_enabled(&softdevice_enabled);
  if (softdevice_enabled != 0) {
    (void)sd_power_gpregret_clr(kRetainedRegisterId, 0xFFUL);
    (void)sd_power_gpregret_set(kRetainedRegisterId, value);
  } else {
    NRF_POWER->GPREGRET2 = value;
  }
}

void startTimer() {
  NRF_RTC2->TASKS_STOP = 1;
  NRF_RTC2->TASKS_CLEAR = 1;
  NRF_RTC2->PRESCALER = kRtcPrescaler;
  NRF_RTC2->CC[0] = kTickCounts;
  NRF_RTC2->EVENTS_COMPARE[0] = 0;
  NRF_RTC2->INTENSET = RTC_INTENSET_COMPARE0_Msk;
  NVIC_SetPriority(RTC2_IRQn, kTimerInterruptPriority);
  NVIC_ClearPendingIRQ(RTC2_IRQn);
  NVIC_EnableIRQ(RTC2_IRQn);
  NRF_RTC2->TASKS_START = 1;
}

#endif  // NRF52_SERIES

}  // namespace

#if defined(NRF52_SERIES)

void LoopHealthMonitor::begin() {
  previous_boot_ = loop_stall_record::decode(readRetained());
  // Always start from an empty record: a later reset for any other reason
  // must not inherit this boot's history.
  writeRetained(loop_stall_record::kEmpty);
  startTimer();
}

uint32_t LoopHealthMonitor::idleClock() { return NRF_RTC2->COUNTER; }

void LoopHealthMonitor::idleReturned(uint32_t clock_before_sleep) {
  const uint32_t counts =
      (NRF_RTC2->COUNTER - clock_before_sleep) & kRtcCounterMask;
  idle_stats.record(counts * (1000UL / kRtcCountsPerSecond));
}

uint32_t LoopHealthMonitor::loopStackFreeBytes() {
  return static_cast<uint32_t>(uxTaskGetStackHighWaterMark(nullptr)) *
         sizeof(StackType_t);
}

void LoopHealthMonitor::onTimerInterrupt() {
  switch (stall_detector.onTick(completed_passes_)) {
    case LoopStallDetector::Action::kCapture:
      writeRetained(loop_stall_record::encodeStall(
          static_cast<LoopStage>(current_stage_)));
      break;
    case LoopStallDetector::Action::kClear:
      writeRetained(loop_stall_record::kEmpty);
      break;
    case LoopStallDetector::Action::kNone:
      break;
  }
}

#else  // host build: no retained register, no timer

void LoopHealthMonitor::begin() { previous_boot_ = LoopStallRecord{}; }
uint32_t LoopHealthMonitor::idleClock() { return 0; }
void LoopHealthMonitor::idleReturned(uint32_t) {}
uint32_t LoopHealthMonitor::loopStackFreeBytes() { return 0; }
void LoopHealthMonitor::onTimerInterrupt() {}

#endif

uint32_t LoopHealthMonitor::idleOversleeps() { return idle_stats.oversleeps(); }
uint32_t LoopHealthMonitor::longestIdleMs() { return idle_stats.longestMs(); }
uint32_t LoopHealthMonitor::recoveredStalls() {
  return stall_detector.recoveredStalls();
}

}  // namespace orun_tlp

#if defined(NRF52_SERIES)

extern "C" {

// Overrides the weak default in the core's startup file.
void RTC2_IRQHandler(void) {
  if (NRF_RTC2->EVENTS_COMPARE[0] != 0) {
    NRF_RTC2->EVENTS_COMPARE[0] = 0;
    // Read back so the event clear has taken effect before the handler
    // returns; otherwise the interrupt can fire a second time.
    (void)NRF_RTC2->EVENTS_COMPARE[0];
    NRF_RTC2->CC[0] = (NRF_RTC2->COUNTER + orun_tlp::kTickCounts) &
                      orun_tlp::kRtcCounterMask;
    orun_tlp::LoopHealthMonitor::onTimerInterrupt();
  }
}

// Overrides the weak default, which only spins. Record that a fault happened
// and where the loop was, then keep spinning so the watchdog resets the MCU
// exactly as before. No SoftDevice call is possible here, so the register is
// written directly; if the SoftDevice rejects that write the resulting
// lock-up reset is itself reported in the reset reason.
void HardFault_Handler(void) {
  NRF_POWER->GPREGRET2 = orun_tlp::loop_stall_record::encodeHardFault(
      static_cast<orun_tlp::LoopStage>(
          orun_tlp::LoopHealthMonitor::currentStageCode()));
  while (true) {
    __NOP();
  }
}

}  // extern "C"

#endif  // NRF52_SERIES
