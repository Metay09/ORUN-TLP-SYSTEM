#pragma once

#include <stdint.h>

#include "loop_health.h"

namespace orun_tlp {

// nRF52 binding for loop_health.h.
//
// - One retained byte (POWER.GPREGRET2) carries a stall/fault record across
//   a watchdog reset. GPREGRET is left alone: the bootloader owns it.
// - RTC2 runs from the 32 kHz clock, independent of the RTOS tick (RTC1) and
//   of the SoftDevice (RTC0). Its interrupt fires every kTickPeriodMs.
// - The interrupt also wakes the MCU, so an idle sleep whose RTOS wake-up was
//   lost ends within one tick period instead of lasting until the watchdog
//   resets the device. Such a sleep is counted, not hidden.
//
// The watchdog stays the final recovery layer and is unchanged.
class LoopHealthMonitor {
 public:
  static constexpr uint32_t kTickPeriodMs = 2000;
  // 20 s without a completed pass. The watchdog fires at 30 s.
  static constexpr uint8_t kCaptureAfterTicks = 10;
  // The idle call asks for 10 ms. Half a second is not scheduling jitter.
  static constexpr uint32_t kIdleOversleepThresholdMs = 500;

  // Reads and erases the previous boot's record, then starts the timer.
  // Call once from setup(), before peripherals are initialized.
  static void begin();

  // Loop-task markers. Cheap: one RAM store each.
  static void enter(LoopStage stage) {
    current_stage_ = static_cast<uint8_t>(stage);
  }
  static void passCompleted() { ++completed_passes_; }

  // Wrap the idle sleep: `before = idleClock(); sleep; idleReturned(before)`.
  static uint32_t idleClock();
  static void idleReturned(uint32_t clock_before_sleep);

  static const LoopStallRecord& previousBoot() { return previous_boot_; }
  static uint32_t completedPasses() { return completed_passes_; }
  static uint32_t idleOversleeps();
  static uint32_t longestIdleMs();
  static uint32_t recoveredStalls();
  // Smallest amount of loop-task stack that has ever been free, in bytes.
  // Walks the unused part of the stack: call on demand, not every pass.
  static uint32_t loopStackFreeBytes();

  // Interrupt entry points (public for the C handlers only).
  static void onTimerInterrupt();
  static uint8_t currentStageCode() { return current_stage_; }

 private:
  static volatile uint8_t current_stage_;
  static volatile uint32_t completed_passes_;
  static LoopStallRecord previous_boot_;
};

}  // namespace orun_tlp
