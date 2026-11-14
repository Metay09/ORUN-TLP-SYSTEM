// 6b/6d: the Gateway's LoRa command mailbox (lora_command_relay.h).
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
  if (!wellFormed(frame, size)) return Queued::kMalformed;
  const uint64_t target = be64(frame + 4);
  const uint32_t cid = be32(frame + 16);
  const bool areas = frame[3] == kResourceAreas;
  const uint8_t part = areas ? frame[32] : 0U;
  const uint8_t parts = areas ? frame[33] : 1U;
  Slot* slot = nullptr;
  Slot* free_slot = nullptr;
  for (Slot& s : slots_) {
    if (s.used && s.target == target) slot = &s;
    if (!s.used && free_slot == nullptr) free_slot = &s;
  }
  Queued result = Queued::kAdded;
  if (slot != nullptr && slot->command_id == cid && slot->parts == parts) {
    result = Queued::kReplaced;  // another part (or a copy) of the same command
  } else {
    if (slot != nullptr) result = Queued::kReplaced;
    else if (free_slot == nullptr) return Queued::kFull;
    else slot = free_slot;
    *slot = Slot();
    slot->used = true;
    slot->target = target;
    slot->command_id = cid;
    slot->parts = parts;
  }
  memcpy(slot->frames[part], frame, size);
  slot->sizes[part] = static_cast<uint8_t>(size);
  slot->have = static_cast<uint8_t>(slot->have | (1U << part));
  return result;
}

void LoraCommandRelay::heard(uint64_t device, uint32_t now) {
  heard_ = device;
  heard_at_ = now;
  heard_open_ = true;
  ack_sent_ = false;
  sent_now_ = 0;
}

void LoraCommandRelay::ackSent(uint32_t now) {
  (void)now;
  if (heard_open_) ack_sent_ = true;
}

bool LoraCommandRelay::due(uint32_t now, Frame& out) {
  if (!heard_open_) return false;
  if (sent_now_ == 0) {
    if (monotonic::elapsed(now, heard_at_, kLatestAfterHeardMs)) {
      heard_open_ = false;  // its window is over: wait for its next report
      return false;
    }
    if (!ack_sent_ && !monotonic::elapsed(now, heard_at_, kAckGraceMs)) return false;
  } else if (monotonic::elapsed(now, last_part_at_, kLatestNextPartMs)) {
    heard_open_ = false;
    return false;
  }
  for (Slot& s : slots_) {
    if (!s.used || s.target != heard_) continue;
    if (sent_now_ == 0 && s.attempt >= kMaxAttempts) break;
    // The next held part not yet sent in this window.
    for (uint8_t p = 0; p < s.parts; ++p) {
      if ((sent_now_ & (1U << p)) != 0U || (s.have & (1U << p)) == 0U) continue;
      // Peek only. A pre-admission failure (e.g. RF duty cap) must not
      // consume this report's try, block later parts, or reach give-up.
      memcpy(out.bytes, s.frames[p], s.sizes[p]);
      out.size = s.sizes[p];
      out.target = s.target;
      out.command_id = s.command_id;
      out.attempt = sent_now_ == 0U
                        ? static_cast<uint8_t>(s.attempt + 1U)
                        : s.attempt;
      out.part = p;
      out.parts = s.parts;
      return true;
    }
    break;
  }
  heard_open_ = false;  // nothing (more) for it in this window
  return false;
}

bool LoraCommandRelay::markTxStarted(uint32_t now,
                                      const Frame& transmitted) {
  if (!heard_open_ || transmitted.target != heard_ ||
      transmitted.part >= kMaxParts ||
      transmitted.size > lora_command::kMaxFrameSize)
    return false;
  const uint8_t part_bit = static_cast<uint8_t>(1U << transmitted.part);
  if ((sent_now_ & part_bit) != 0U) return false;
  for (Slot& s : slots_) {
    if (!s.used || s.target != transmitted.target ||
        s.command_id != transmitted.command_id ||
        s.parts != transmitted.parts ||
        (s.have & part_bit) == 0U ||
        s.sizes[transmitted.part] != transmitted.size ||
        memcmp(s.frames[transmitted.part], transmitted.bytes,
               transmitted.size) != 0) continue;

    if (sent_now_ == 0U) {
      if (s.attempt >= kMaxAttempts ||
          transmitted.attempt != static_cast<uint8_t>(s.attempt + 1U))
        return false;
      ++s.attempt;
    } else if (transmitted.attempt != s.attempt) {
      return false;
    }
    sent_now_ = static_cast<uint8_t>(sent_now_ | part_bit);
    last_part_at_ = now;
    return true;
  }
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
    if (s.used && s.target == device && s.command_id == id) {
      s.used = false;
      return true;
    }
  }
  return false;
}

bool LoraCommandRelay::takeDropped(uint64_t& target, uint32_t& command_id) {
  // A command whose last try went out and whose Tracker has been heard again
  // since (so the answer had its window) without a RESULT is given up.
  if (!heard_open_ || sent_now_ != 0) return false;
  for (Slot& s : slots_) {
    if (s.used && s.attempt >= kMaxAttempts && s.target == heard_) {
      s.used = false;
      target = s.target;
      command_id = s.command_id;
      return true;
    }
  }
  return false;
}

size_t LoraCommandRelay::count() const {
  size_t n = 0;
  for (const Slot& s : slots_) n += s.used ? 1U : 0U;
  return n;
}

}  // namespace orun_tlp
