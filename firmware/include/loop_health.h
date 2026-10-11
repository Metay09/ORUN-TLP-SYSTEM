#pragma once

#include <stdint.h>

namespace orun_tlp {

// Reset attribution for the cooperative loop.
//
// The watchdog is fed once per loop pass, so "reset reason = watchdog" only
// says that one pass never finished. It does not say where. These portable
// pieces answer three different questions about such a reset:
//
//   1. Was the loop stuck inside one of its own steps, and which one?
//   2. Did the MCU fault instead (the default handler just spins)?
//   3. Did the loop simply not wake up from its idle sleep?
//
// Hardware binding (retained register, independent timer, fault handler)
// lives in loop_health_monitor.cpp. Nothing here is a wire or flash format.

// The loop step that was running. The numeric value is stored in a retained
// 6-bit field: append only, never renumber, never exceed 63.
enum class LoopStage : uint8_t {
  kNone = 0,            // setup(), before the first pass
  kGnss = 1,
  kAccelerometer = 2,
  kActivity = 3,
  kUsbCommands = 4,
  kServices = 5,        // runtime service resolution, relay intent
  kBle = 6,
  kFlashEvents = 7,
  kHistoryStore = 8,
  kConfigStore = 9,
  kSecurityStore = 10,
  kConfigMutation = 11,
  kPositionFlow = 12,
  kFixHandling = 13,    // location, geofence, store-before-send
  kRadio = 14,
  kStoreForward = 15,
  kIdle = 16,           // PowerManager::idle(): asleep or pre-empted
  kBattery = 17,        // battery voltage ADC reading
};

constexpr uint8_t kLoopStageMaxValue = 63;

// Short stable name for diagnostics. Unknown codes print as "UNKNOWN".
const char* loopStageName(LoopStage stage);

// What the previous boot left behind in the one retained byte.
struct LoopStallRecord {
  // The independent timer saw no completed loop pass for the capture window
  // while interrupts were still being served. `stage` is where the loop was.
  bool stall_captured = false;
  // The HardFault handler ran. `stage` is where the loop was at that moment.
  bool hard_fault = false;
  LoopStage stage = LoopStage::kNone;
};

namespace loop_stall_record {
constexpr uint8_t kStallCapturedBit = 0x80;
constexpr uint8_t kHardFaultBit = 0x40;
constexpr uint8_t kStageMask = 0x3F;
// Zero means "nothing recorded": a normal boot must leave the byte at zero.
constexpr uint8_t kEmpty = 0x00;

uint8_t encodeStall(LoopStage stage);
uint8_t encodeHardFault(LoopStage stage);
LoopStallRecord decode(uint8_t value);
}  // namespace loop_stall_record

// Fed from a periodic interrupt that does not depend on the RTOS tick.
class LoopStallDetector {
 public:
  enum class Action : uint8_t {
    kNone,
    // No pass completed for `capture_after_ticks` ticks: persist the stage.
    kCapture,
    // A captured stall ended without a reset: erase the persisted record so
    // a later unrelated reset is not blamed on it.
    kClear,
  };

  explicit constexpr LoopStallDetector(uint8_t capture_after_ticks)
      : capture_after_ticks_(capture_after_ticks) {}

  Action onTick(uint32_t completed_passes);

  // Stalls that were captured and then ended on their own.
  uint32_t recoveredStalls() const { return recovered_stalls_; }

 private:
  uint8_t capture_after_ticks_;
  uint8_t stalled_ticks_ = 0;
  bool captured_ = false;
  bool seen_any_ = false;
  uint32_t last_completed_passes_ = 0;
  uint32_t recovered_stalls_ = 0;
};

// How long PowerManager::idle() really took, measured on the independent
// timer. A sleep that overruns by far means the RTOS wake-up was missed and
// something else (the independent timer) ended it.
class IdleSleepStats {
 public:
  explicit constexpr IdleSleepStats(uint32_t oversleep_threshold_ms)
      : oversleep_threshold_ms_(oversleep_threshold_ms) {}

  void record(uint32_t measured_ms);

  uint32_t oversleeps() const { return oversleeps_; }
  uint32_t longestMs() const { return longest_ms_; }

 private:
  uint32_t oversleep_threshold_ms_;
  uint32_t oversleeps_ = 0;
  uint32_t longest_ms_ = 0;
};

}  // namespace orun_tlp
