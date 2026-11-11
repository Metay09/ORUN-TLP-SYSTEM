// 6b: the Gateway's LoRa command mailbox (lora_command_relay.h).
#include "lora_command_relay.h"

#include <string.h>

#include "monotonic_time.h"

namespace orun_tlp {

namespace {
uint64_t be64(const uint8_t* p) {
  uint64_t v = 0;
  for (int i = 0; i < 8; ++i) v = (v << 8) | p[i];
  return v;
}
uint32_t be32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}
}  // namespace

LoraCommandRelay::Queued LoraCommandRelay::queue(const uint8_t* frame, size_t size) {
  using namespace lora_command;
  // Shape only: the Gateway cannot (and must not) judge the signature.
  if (!isCommandFrame(frame, size) || size < kMinCommandSize || size > kMaxCommandSize ||
      size != kHeaderSize + size_t(frame[32]) * kEntrySize + kTagSize)
    return Queued::kMalformed;
  const uint64_t target = be64(frame + 4);
  Slot* free_slot = nullptr;
  for (Slot& s : slots_) {
    if (s.used && s.frame.target == target) {
      memcpy(s.frame.bytes, frame, size);
      s.frame.size = static_cast<uint8_t>(size);
      s.frame.command_id = be32(frame + 16);
      s.frame.attempt = 0;
      return Queued::kReplaced;
    }
    if (!s.used && free_slot == nullptr) free_slot = &s;
  }
  if (free_slot == nullptr) return Queued::kFull;
  free_slot->used = true;
  memcpy(free_slot->frame.bytes, frame, size);
  free_slot->frame.size = static_cast<uint8_t>(size);
  free_slot->frame.target = target;
  free_slot->frame.command_id = be32(frame + 16);
  free_slot->frame.attempt = 0;
  return Queued::kAdded;
}

void LoraCommandRelay::heard(uint64_t device, uint32_t now) {
  heard_ = device;
  heard_at_ = now;
  heard_open_ = true;
  ack_sent_ = false;
}

void LoraCommandRelay::ackSent(uint32_t now) {
  (void)now;
  if (heard_open_) ack_sent_ = true;
}

bool LoraCommandRelay::due(uint32_t now, Frame& out) {
  if (!heard_open_) return false;
  if (monotonic::elapsed(now, heard_at_, kLatestAfterHeardMs)) {
    heard_open_ = false;  // its window is over: wait for its next report
    return false;
  }
  if (!ack_sent_ && !monotonic::elapsed(now, heard_at_, kAckGraceMs)) return false;
  for (Slot& s : slots_) {
    if (!s.used || s.frame.target != heard_ || s.frame.attempt >= kMaxAttempts) continue;
    heard_open_ = false;  // one try per report
    ++s.frame.attempt;
    out = s.frame;
    return true;
  }
  heard_open_ = false;
  return false;
}

bool LoraCommandRelay::answered(const uint8_t* result, size_t size) {
  using namespace lora_command;
  if (result == nullptr || size != kResultSize || result[0] != kVersion ||
      result[1] != kResultType)
    return false;
  const uint64_t device = be64(result + 4);
  const uint32_t id = be32(result + 16);
  for (Slot& s : slots_) {
    if (s.used && s.frame.target == device && s.frame.command_id == id) {
      s.used = false;
      return true;
    }
  }
  return false;
}

bool LoraCommandRelay::takeDropped(uint64_t& target, uint32_t& command_id) {
  // A command whose last try went out and whose Tracker has been heard again
  // since (so the answer had its window) without a RESULT is given up.
  if (!dropped_) {
    for (Slot& s : slots_) {
      if (s.used && s.frame.attempt >= kMaxAttempts && heard_open_ &&
          heard_ == s.frame.target) {
        s.used = false;
        dropped_ = true;
        dropped_target_ = s.frame.target;
        dropped_id_ = s.frame.command_id;
        break;
      }
    }
  }
  if (!dropped_) return false;
  dropped_ = false;
  target = dropped_target_;
  command_id = dropped_id_;
  return true;
}

size_t LoraCommandRelay::count() const {
  size_t n = 0;
  for (const Slot& s : slots_) n += s.used ? 1U : 0U;
  return n;
}

}  // namespace orun_tlp
