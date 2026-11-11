#pragma once

// 6b (owner decision 2026-11-11): the Gateway carries account-signed LoRa
// commands (lora_command.h) to sleepy Trackers. It holds NO authority: it
// never reads, changes or signs a command, it only keeps the exact bytes a
// bridge (PC Edge over USB, later the phone) handed it until the target
// Tracker is heard, then sends them in that Tracker's listening window right
// after its report (and after the custody ACK). The Tracker's signed RESULT
// goes back to the bridge as is; the bridge/server verifies it.
//
// A command waits for at most kMaxAttempts reports (each send is one try);
// then it is dropped and the bridge is told. One command per Tracker: a
// newer one for the same Tracker replaces the older one. RAM only: after a
// Gateway restart the bridge hands the commands again.

#include <stddef.h>
#include <stdint.h>

#include "lora_command.h"

namespace orun_tlp {

class LoraCommandRelay {
 public:
  static constexpr size_t kSlots = 8;
  static constexpr uint8_t kMaxAttempts = 6;
  // After hearing a Tracker, wait for its custody ACK to go first; send once
  // the ACK went out (or this long after hearing it if no ACK is coming)...
  static constexpr uint32_t kAckGraceMs = 2500;
  // ...but never later than this after its report (the Tracker listens ~10 s).
  static constexpr uint32_t kLatestAfterHeardMs = 7000;

  enum class Queued : uint8_t { kAdded, kReplaced, kMalformed, kFull };

  struct Frame {
    uint8_t bytes[lora_command::kMaxCommandSize];
    uint8_t size;
    uint64_t target;
    uint32_t command_id;
    uint8_t attempt;  // 1-based, of kMaxAttempts
  };

  Queued queue(const uint8_t* frame, size_t size);

  // A frame from this Tracker was just received (its report).
  void heard(uint64_t device, uint32_t now);
  // The custody ACK to the Tracker last heard has gone out.
  void ackSent(uint32_t now);

  // The command to send now, if any (call only when the radio can send).
  // Counts the attempt.
  bool due(uint32_t now, Frame& out);

  // A RESULT came back: true if it answered a queued command (removed).
  // Any well-formed RESULT is passed on by the caller either way.
  bool answered(const uint8_t* result, size_t size);

  // A command that used up its attempts (removed); one per call.
  bool takeDropped(uint64_t& target, uint32_t& command_id);

  size_t count() const;

 private:
  struct Slot {
    bool used = false;
    Frame frame{};
  };
  Slot slots_[kSlots];
  uint64_t heard_ = 0;
  uint32_t heard_at_ = 0;
  bool heard_open_ = false;
  bool ack_sent_ = false;
  bool dropped_ = false;
  uint64_t dropped_target_ = 0;
  uint32_t dropped_id_ = 0;
};

}  // namespace orun_tlp
