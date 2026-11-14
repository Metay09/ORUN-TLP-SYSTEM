#pragma once

#include <stddef.h>
#include <stdint.h>

namespace orun_tlp {

// Shared per-device RF airtime ledger; callers must gate every physical LoRa TX.
// The 61 one-minute bins deliberately over-count at window boundaries: this
// protects every rolling 60-minute interval, rather than wall-clock hours.
// Project rule: every power-on starts with a full, fresh RAM budget and no
// start-up wait. This per-boot rule does not track the previous power cycle.
// Matches the SX126x SetTxConfig semantic LoRa parameters, not driver codes.
struct LoRaAirtimeProfile {
  uint8_t spreading_factor = 11U;
  uint32_t bandwidth_hz = 125000U;
  uint8_t coding_rate = 1U;  // 1 => 4/5 ... 4 => 4/8
  uint16_t preamble_symbols = 8U;
  bool crc_enabled = true;
  bool explicit_header = true;
};

// Conservative ceil(ms) based on the Semtech LoRa time-on-air equation.
// Zero means invalid frame/profile; callers must fail closed.
uint32_t loraAirtimeMs(size_t payload_bytes,
                       const LoRaAirtimeProfile& profile);

class RfDutyBudget final {
 public:
  enum class Frame : uint8_t {
    kPosition, kHistorySecure, kProductSecure, kRelay,
    kCustodyAck, kLoraCommandResult, kTest
  };
  enum class Decision : uint8_t {
    kAllowed, kExhausted, kClockFault, kBadFrame, kBadAirtime
  };

  static constexpr uint32_t kHourMs = 3600000U;
  static constexpr uint32_t kMaxAirtimeMs = 360000U;
  static constexpr uint32_t kBucketMs = 60000U;
  static constexpr uint8_t kBucketCount = 61U;

  void begin(uint32_t now_ms);
  Decision canReserve(uint32_t now_ms, uint32_t estimated_airtime_ms, Frame frame);
  Decision reserve(uint32_t now_ms, uint32_t estimated_airtime_ms, Frame frame);
  uint32_t usedMs() const;
  bool faulted() const { return clock_fault_; }

 private:
  struct Bucket {
    uint64_t minute = UINT64_MAX;
    uint32_t airtime_ms = 0U;
  };
  Bucket bins_[kBucketCount]{};
  uint64_t elapsed_ms_ = 0U;
  uint32_t last_ms_ = 0U;
  bool started_ = false;
  bool clock_fault_ = false;
  uint32_t used_at_current_minute_ = 0U;
  bool stepClock(uint32_t now_ms);
  uint32_t totalInConservativeWindow(uint64_t current_minute) const;
};

}  // namespace orun_tlp
