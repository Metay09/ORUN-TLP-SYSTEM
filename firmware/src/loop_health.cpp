#include "loop_health.h"

namespace orun_tlp {

const char* loopStageName(LoopStage stage) {
  switch (stage) {
    case LoopStage::kNone: return "NONE";
    case LoopStage::kGnss: return "GNSS";
    case LoopStage::kAccelerometer: return "ACCEL";
    case LoopStage::kActivity: return "ACTIVITY";
    case LoopStage::kUsbCommands: return "USB";
    case LoopStage::kServices: return "SERVICES";
    case LoopStage::kBle: return "BLE";
    case LoopStage::kFlashEvents: return "FLASH_EVENTS";
    case LoopStage::kHistoryStore: return "HISTORY";
    case LoopStage::kConfigStore: return "CONFIG";
    case LoopStage::kSecurityStore: return "SECURITY";
    case LoopStage::kConfigMutation: return "CONFIG_MUTATION";
    case LoopStage::kPositionFlow: return "POSITION";
    case LoopStage::kFixHandling: return "FIX";
    case LoopStage::kRadio: return "RADIO";
    case LoopStage::kStoreForward: return "STORE_FORWARD";
    case LoopStage::kIdle: return "IDLE";
    case LoopStage::kBattery: return "BATTERY";
  }
  return "UNKNOWN";
}

namespace loop_stall_record {

uint8_t encodeStall(LoopStage stage) {
  return static_cast<uint8_t>(kStallCapturedBit |
                              (static_cast<uint8_t>(stage) & kStageMask));
}

uint8_t encodeHardFault(LoopStage stage) {
  return static_cast<uint8_t>(kHardFaultBit |
                              (static_cast<uint8_t>(stage) & kStageMask));
}

LoopStallRecord decode(uint8_t value) {
  LoopStallRecord record;
  record.stall_captured = (value & kStallCapturedBit) != 0;
  record.hard_fault = (value & kHardFaultBit) != 0;
  // Without either flag the byte carries no stage: a stray low value left by
  // something else must not be read as "stuck in that stage".
  if (record.stall_captured || record.hard_fault)
    record.stage = static_cast<LoopStage>(value & kStageMask);
  return record;
}

}  // namespace loop_stall_record

LoopStallDetector::Action LoopStallDetector::onTick(
    uint32_t completed_passes) {
  if (!seen_any_ || completed_passes != last_completed_passes_) {
    // Progress (or the very first tick, which only sets the baseline).
    seen_any_ = true;
    last_completed_passes_ = completed_passes;
    stalled_ticks_ = 0;
    if (captured_) {
      captured_ = false;
      ++recovered_stalls_;
      return Action::kClear;
    }
    return Action::kNone;
  }

  if (captured_) return Action::kNone;
  if (stalled_ticks_ < capture_after_ticks_) ++stalled_ticks_;
  if (stalled_ticks_ >= capture_after_ticks_) {
    captured_ = true;
    return Action::kCapture;
  }
  return Action::kNone;
}

void IdleSleepStats::record(uint32_t measured_ms) {
  if (measured_ms > longest_ms_) longest_ms_ = measured_ms;
  if (measured_ms >= oversleep_threshold_ms_ && oversleeps_ != UINT32_MAX)
    ++oversleeps_;
}

}  // namespace orun_tlp
