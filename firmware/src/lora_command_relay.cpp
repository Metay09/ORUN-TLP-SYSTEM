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
    // Replacement cannot inherit the old TX window.
    if (slot->used && target == heard_ && sent_now_) {
      heard_open_ = false;
      sent_now_ = 0;
      awaiting_terminal_ = false;
      last_part_rx_floor_ms_ = 0;
    }

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
  awaiting_terminal_ = false;
  last_part_rx_floor_ms_ = 0;
}

void LoraCommandRelay::ackSent(uint32_t now) {
  (void)now;
  if (heard_open_) ack_sent_ = true;
}


void LoraCommandRelay::ackSentFor(
    uint64_t device, uint32_t rx_floor_ms) {
  if (heard_open_ && device == heard_ &&
      rx_floor_ms == heard_at_)
    ack_sent_ = true;
}

void LoraCommandRelay::onTxResult(
    uint64_t target, uint32_t command_id,
    uint8_t part, bool succeeded) {
  if (!awaiting_terminal_ ||
      target != heard_ ||
      command_id != active_command_id_ ||
      part != active_part_)
    return;

  awaiting_terminal_ = false;

  // Timeout cannot establish a new RX window.
  if (!succeeded) heard_open_ = false;
}

bool LoraCommandRelay::trySend(
    uint32_t now, Frame& out,
    bool (*airtime_available)(uint8_t),
    bool (*start_tx)(const Frame&)) {
  if (!start_tx || !heard_open_ ||
      awaiting_terminal_) return false;
  if (sent_now_ == 0) {
    if (monotonic::elapsed(now, heard_at_, kLatestAfterHeardMs)) {
      heard_open_ = false;  // its window is over: wait for its next report
      return false;
    }
    if (!ack_sent_ && !monotonic::elapsed(now, heard_at_, kAckGraceMs)) return false;
  } else {
    if (!monotonic::reached(
            now, last_part_rx_floor_ms_)) return false;

    if (monotonic::elapsed(
            now, last_part_rx_floor_ms_,
            kLatestNextPartMs)) {
      heard_open_ = false;
      return false;
    }
  }
  for (Slot& s : slots_) {
    if (!s.used || s.target != heard_) continue;
    if (sent_now_ == 0 && s.attempt >= kMaxAttempts) break;
    // The next held part not yet sent in this window.
    for (uint8_t p = 0; p < s.parts; ++p) {
      if ((sent_now_ & (1U << p)) != 0U) continue;

      // Never skip a missing earlier part.
      if ((s.have & (1U << p)) == 0U) return false;
      const uint32_t floor =
          sent_now_ ? last_part_rx_floor_ms_ : heard_at_;

      if (!ackFitsWindow(now, floor, s.sizes[p]))
        return false;

      if (airtime_available &&
          !airtime_available(s.sizes[p])) return false;
      memcpy(out.bytes, s.frames[p], s.sizes[p]);
      out.size = s.sizes[p];
      out.target = s.target;
      out.command_id = s.command_id;
      out.attempt = static_cast<uint8_t>(
          s.attempt + (sent_now_ == 0 ? 1U : 0U));
      out.part = p;
      out.parts = s.parts;
      out.rx_floor_ms = floor;

      // A failed admission must leave all command state intact.
      if (!start_tx(out)) return false;

      if (sent_now_ == 0) ++s.attempt;
      sent_now_ = static_cast<uint8_t>(sent_now_ | (1U << p));
      active_command_id_ = s.command_id;
      active_part_ = p;
      awaiting_terminal_ = true;
      last_part_rx_floor_ms_ =
          now + profileAirtimeUs(out.size) / 1000U;
      return true;
    }
    break;
  }
  heard_open_ = false;  // nothing (more) for it in this window
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
      if (device == heard_) {
        heard_open_ = false;
        sent_now_ = 0;
        awaiting_terminal_ = false;
      }
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
