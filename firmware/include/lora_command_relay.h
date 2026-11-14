#pragma once

// 6b/6d (owner decision 2026-11-11): the Gateway carries account-signed LoRa
// commands (lora_command.h) to sleepy Trackers. It holds NO authority: it
// never reads, changes or signs a command, it only keeps the exact frames a
// bridge (PC Edge or the phone, over USB or Bluetooth) handed it until the
// target Tracker is heard, then sends them in that Tracker's listening window
// right after its report (and after the custody ACK). An area list may be up
// to three frames ("parts"); they go back to back in the same window. The
// Tracker's signed RESULT goes back to the bridge as is.
//
// A command waits for at most kMaxAttempts reports (each report is one try);
// then it is dropped and the bridge is told. One command per Tracker: a
// newer one (another command id) for the same Tracker replaces the older one.
// RAM only: after a Gateway restart the bridge hands the commands again.

#include <stddef.h>
#include <stdint.h>

#include "lora_command.h"

namespace orun_tlp {

class LoraCommandRelay {
 public:
  static constexpr size_t kSlots = 8;
  static constexpr uint8_t kMaxAttempts = 6;
  static constexpr uint8_t kMaxParts = lora_command::kMaxAreaParts;
  // After hearing a Tracker, wait for its custody ACK to go first; send once
  // the ACK went out (or this long after hearing it if no ACK is coming)...
  static constexpr uint32_t kAckGraceMs = 2500;
  // ...but start no later than this after its report (it listens ~10 s; each
  // part it receives keeps it listening a while longer).
  static constexpr uint32_t kLatestAfterHeardMs = 7000;
  static constexpr uint32_t kLatestNextPartMs = 20000;

  enum class Queued : uint8_t { kAdded, kReplaced, kMalformed, kFull };

  struct Frame {
    uint8_t bytes[lora_command::kMaxFrameSize];
    uint8_t size;
    uint64_t target;
    uint32_t command_id;
    uint8_t attempt;  // 1-based, of kMaxAttempts
    uint8_t part;     // 0-based
    uint8_t parts;
  };

  // One frame of a command (all parts of an area list come as separate frames).
  Queued queue(const uint8_t* frame, size_t size);

  // A frame from this Tracker was just received (its report).
  void heard(uint64_t device, uint32_t now);
  // The custody ACK to the Tracker last heard has gone out.
  void ackSent(uint32_t now);

  // A read-only candidate: does NOT spend a report attempt or mark the part
  // sent. RadioManager may reject the frame due to RF duty exhaustion.
  bool due(uint32_t now, Frame& out);
  // Call ONLY after RadioManager confirms that Radio.Send began. False means
  // an unexpected owner/queue mismatch and must be reported as a fault.
  bool markTxStarted(uint32_t now, const Frame& transmitted);

  // A RESULT came back: true if it answered a queued command (removed).
  bool answered(const uint8_t* result, size_t size);

  // A command that used up its attempts (removed); one per call.
  bool takeDropped(uint64_t& target, uint32_t& command_id);

  size_t count() const;

 private:
  struct Slot {
    bool used = false;
    uint64_t target = 0;
    uint32_t command_id = 0;
    uint8_t parts = 0;
    uint8_t have = 0;     // bit per held part
    uint8_t attempt = 0;
    uint8_t sizes[kMaxParts] = {};
    uint8_t frames[kMaxParts][lora_command::kMaxFrameSize] = {};
  };
  Slot slots_[kSlots];
  uint64_t heard_ = 0;
  uint32_t heard_at_ = 0;
  bool heard_open_ = false;
  bool ack_sent_ = false;
  uint8_t sent_now_ = 0;      // parts already sent in this window
  uint32_t last_part_at_ = 0;
};

}  // namespace orun_tlp
