// Loop health: which loop step a watchdog reset interrupted.
//
// Part 1 checks the portable record/detector/statistics. Part 2 compiles the
// real nRF binding against register fakes and drives its interrupt handler:
// what is written to the retained register, through which path, and when.
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "Arduino.h"
#include "nrf.h"
#include "nrf_soc.h"
#include "loop_health.h"
#include "loop_health_monitor.h"

extern "C" void RTC2_IRQHandler(void);

using namespace orun_tlp;

namespace {

void portableRecord() {
  // Empty byte: nothing recorded, and no stage is invented.
  LoopStallRecord none = loop_stall_record::decode(0x00);
  assert(!none.stall_captured && !none.hard_fault);
  assert(none.stage == LoopStage::kNone);

  // A stray low value without a flag is not "stuck in that stage".
  LoopStallRecord stray = loop_stall_record::decode(0x08);
  assert(!stray.stall_captured && !stray.hard_fault);
  assert(stray.stage == LoopStage::kNone);

  const uint8_t stall = loop_stall_record::encodeStall(LoopStage::kHistoryStore);
  assert(stall == 0x88);
  LoopStallRecord a = loop_stall_record::decode(stall);
  assert(a.stall_captured && !a.hard_fault);
  assert(a.stage == LoopStage::kHistoryStore);

  const uint8_t fault = loop_stall_record::encodeHardFault(LoopStage::kIdle);
  assert(fault == 0x50);
  LoopStallRecord b = loop_stall_record::decode(fault);
  assert(!b.stall_captured && b.hard_fault && b.stage == LoopStage::kIdle);

  // Every stage round-trips, has a distinct printable name and fits 6 bits.
  const LoopStage stages[] = {
      LoopStage::kNone,         LoopStage::kGnss,
      LoopStage::kAccelerometer, LoopStage::kActivity,
      LoopStage::kUsbCommands,  LoopStage::kServices,
      LoopStage::kBle,          LoopStage::kFlashEvents,
      LoopStage::kHistoryStore, LoopStage::kConfigStore,
      LoopStage::kSecurityStore, LoopStage::kConfigMutation,
      LoopStage::kPositionFlow, LoopStage::kFixHandling,
      LoopStage::kRadio,        LoopStage::kStoreForward,
      LoopStage::kIdle};
  const unsigned count = sizeof(stages) / sizeof(stages[0]);
  for (unsigned i = 0; i < count; ++i) {
    assert(static_cast<uint8_t>(stages[i]) <= kLoopStageMaxValue);
    assert(loop_stall_record::decode(
               loop_stall_record::encodeStall(stages[i])).stage == stages[i]);
    assert(strcmp(loopStageName(stages[i]), "UNKNOWN") != 0);
    for (unsigned j = i + 1; j < count; ++j)
      assert(strcmp(loopStageName(stages[i]), loopStageName(stages[j])) != 0);
  }
  assert(strcmp(loopStageName(static_cast<LoopStage>(63)), "UNKNOWN") == 0);
  // Retained codes are frozen: these two anchor the numbering.
  assert(static_cast<uint8_t>(LoopStage::kGnss) == 1);
  assert(static_cast<uint8_t>(LoopStage::kIdle) == 16);
}

void portableDetector() {
  using Action = LoopStallDetector::Action;
  LoopStallDetector detector(3);

  // The first tick only sets the baseline, even if nothing ran yet.
  assert(detector.onTick(0) == Action::kNone);
  // Progress on every tick: never a capture.
  for (uint32_t passes = 1; passes < 50; ++passes)
    assert(detector.onTick(passes) == Action::kNone);

  // No progress: capture on exactly the third stalled tick, once.
  assert(detector.onTick(49) == Action::kNone);
  assert(detector.onTick(49) == Action::kNone);
  assert(detector.onTick(49) == Action::kCapture);
  for (int i = 0; i < 20; ++i) assert(detector.onTick(49) == Action::kNone);
  assert(detector.recoveredStalls() == 0);

  // The loop came back without a reset: erase the record, count it.
  assert(detector.onTick(50) == Action::kClear);
  assert(detector.recoveredStalls() == 1);
  assert(detector.onTick(51) == Action::kNone);

  // A short pause below the threshold leaves no trace.
  assert(detector.onTick(51) == Action::kNone);
  assert(detector.onTick(51) == Action::kNone);
  assert(detector.onTick(52) == Action::kNone);
  assert(detector.recoveredStalls() == 1);

  // A second stall is captured again; the pass counter may wrap.
  LoopStallDetector wrap(2);
  assert(wrap.onTick(UINT32_MAX) == Action::kNone);
  assert(wrap.onTick(0) == Action::kNone);
  assert(wrap.onTick(0) == Action::kNone);
  assert(wrap.onTick(0) == Action::kCapture);
}

void portableIdleStats() {
  IdleSleepStats stats(500);
  stats.record(0);
  stats.record(125);
  stats.record(375);
  assert(stats.oversleeps() == 0 && stats.longestMs() == 375);
  stats.record(500);
  stats.record(2000);
  assert(stats.oversleeps() == 2 && stats.longestMs() == 2000);
  stats.record(10);
  assert(stats.oversleeps() == 2 && stats.longestMs() == 2000);
}

void tick() {
  NRF_RTC2->EVENTS_COMPARE[0] = 1;
  RTC2_IRQHandler();
}

void nrfBinding() {
  using Monitor = LoopHealthMonitor;

  // The previous boot captured a stall in the history step. SoftDevice is
  // still disabled at this point of setup(), so the register is read direct.
  fake_softdevice_enabled = 0;
  fake_nrf_power.GPREGRET = 0x57;  // bootloader's register: never touched
  fake_nrf_power.GPREGRET2 = 0x88;
  Monitor::begin();
  assert(Monitor::previousBoot().stall_captured);
  assert(!Monitor::previousBoot().hard_fault);
  assert(Monitor::previousBoot().stage == LoopStage::kHistoryStore);
  // The record is consumed: a later unrelated reset must not inherit it.
  assert(fake_nrf_power.GPREGRET2 == 0x00);
  assert(fake_nrf_power.GPREGRET == 0x57);
  assert(fake_softdevice_gpregret_calls == 0);

  // 8 Hz independent timer, first interrupt after two seconds, application
  // priority that may call the SoftDevice.
  assert(NRF_RTC2->TASKS_STOP == 1 && NRF_RTC2->TASKS_CLEAR == 1);
  assert(NRF_RTC2->PRESCALER == 4095);
  assert(NRF_RTC2->CC[0] == 16);
  assert(NRF_RTC2->INTENSET == RTC_INTENSET_COMPARE0_Msk);
  assert(fake_nvic_priority_irq == RTC2_IRQn && fake_nvic_priority == 6);
  assert(fake_nvic_cleared_irq == RTC2_IRQn);
  assert(fake_nvic_enabled_irq == RTC2_IRQn);
  assert(NRF_RTC2->TASKS_START == 1);
  assert(Monitor::kTickPeriodMs * Monitor::kCaptureAfterTicks == 20000);

  // A call without the compare event does nothing at all.
  NRF_RTC2->COUNTER = 100;
  NRF_RTC2->EVENTS_COMPARE[0] = 0;
  RTC2_IRQHandler();
  assert(NRF_RTC2->CC[0] == 16);

  // A healthy loop: every tick sees new passes, nothing is written, the
  // event is acknowledged and the next interrupt is armed two seconds on.
  for (int i = 0; i < 30; ++i) {
    Monitor::enter(LoopStage::kGnss);
    Monitor::passCompleted();
    Monitor::enter(LoopStage::kIdle);
    NRF_RTC2->COUNTER = 100 + static_cast<uint32_t>(i) * 16;
    tick();
    assert(NRF_RTC2->EVENTS_COMPARE[0] == 0);
    assert(NRF_RTC2->CC[0] == NRF_RTC2->COUNTER + 16);
    assert(fake_nrf_power.GPREGRET2 == 0x00);
  }

  // The loop stops inside the radio step. Nine silent ticks: nothing yet.
  Monitor::enter(LoopStage::kRadio);
  for (int i = 0; i < 9; ++i) {
    tick();
    assert(fake_nrf_power.GPREGRET2 == 0x00);
  }
  // Tenth tick (20 s, ten seconds before the watchdog): stage is persisted.
  tick();
  assert(fake_nrf_power.GPREGRET2 == 0x8E);
  assert(loop_stall_record::decode(0x8E).stage == LoopStage::kRadio);
  // Further ticks do not rewrite it, even if the marker changes.
  Monitor::enter(LoopStage::kIdle);
  tick();
  assert(fake_nrf_power.GPREGRET2 == 0x8E);
  assert(fake_softdevice_gpregret_calls == 0);

  // The stall ends without a reset: the record is erased and counted.
  Monitor::passCompleted();
  tick();
  assert(fake_nrf_power.GPREGRET2 == 0x00);
  assert(Monitor::recoveredStalls() == 1);

  // With the SoftDevice enabled POWER is written only through its API, and
  // only GPREGRET2 (id 1); the direct register is left alone.
  fake_softdevice_enabled = 1;
  fake_softdevice_gpregret[0] = 0x57;
  fake_softdevice_gpregret[1] = 0x00;
  Monitor::enter(LoopStage::kStoreForward);
  for (int i = 0; i < 10; ++i) tick();
  assert(fake_softdevice_gpregret[1] == 0x8F);
  assert(fake_softdevice_gpregret[0] == 0x57);
  assert(fake_nrf_power.GPREGRET2 == 0x00);
  assert(fake_softdevice_gpregret_calls == 2);  // one clear, one set
  Monitor::passCompleted();
  tick();
  assert(fake_softdevice_gpregret[1] == 0x00);
  assert(Monitor::recoveredStalls() == 2);
  fake_softdevice_enabled = 0;

  // The compare value wraps with the 24-bit counter.
  Monitor::passCompleted();
  NRF_RTC2->COUNTER = 0x00FFFFF8;
  tick();
  assert(NRF_RTC2->CC[0] == 0x00000008);

  // Idle sleep measured on the independent clock: 125 ms per count.
  NRF_RTC2->COUNTER = 1000;
  uint32_t before = Monitor::idleClock();
  assert(before == 1000);
  Monitor::idleReturned(before);              // 0 counts
  NRF_RTC2->COUNTER = 1001;
  Monitor::idleReturned(before);              // 125 ms: ordinary jitter
  assert(Monitor::idleOversleeps() == 0 && Monitor::longestIdleMs() == 125);
  NRF_RTC2->COUNTER = 1016;
  Monitor::idleReturned(before);              // 2 s: the wake-up was missed
  assert(Monitor::idleOversleeps() == 1 && Monitor::longestIdleMs() == 2000);
  // Across the 24-bit wrap.
  NRF_RTC2->COUNTER = 0x00FFFFFE;
  before = Monitor::idleClock();
  NRF_RTC2->COUNTER = 0x00000001;
  Monitor::idleReturned(before);              // 3 counts = 375 ms
  assert(Monitor::idleOversleeps() == 1 && Monitor::longestIdleMs() == 2000);

  fake_stack_high_water_words = 300;
  assert(Monitor::loopStackFreeBytes() == 1200);

  // A boot after a hard fault, and a boot with nothing recorded.
  fake_nrf_power.GPREGRET2 = 0x50;
  Monitor::begin();
  assert(Monitor::previousBoot().hard_fault);
  assert(!Monitor::previousBoot().stall_captured);
  assert(Monitor::previousBoot().stage == LoopStage::kIdle);
  assert(fake_nrf_power.GPREGRET2 == 0x00);
  Monitor::begin();
  assert(!Monitor::previousBoot().hard_fault);
  assert(!Monitor::previousBoot().stall_captured);
}

}  // namespace

int main() {
  portableRecord();
  portableDetector();
  portableIdleStats();
  nrfBinding();
  puts("R4 loop health stall record, detector and nRF binding checks: PASS");
}
