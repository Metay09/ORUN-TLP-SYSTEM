#include "sf5k1_batch_ack_candidate.h"
#include <string.h>

namespace orun_tlp {
namespace sf5k1_candidate {
namespace {
void put32(uint8_t* b, uint32_t v) {
  b[0] = static_cast<uint8_t>(v >> 24);
  b[1] = static_cast<uint8_t>(v >> 16);
  b[2] = static_cast<uint8_t>(v >> 8);
  b[3] = static_cast<uint8_t>(v);
}
void put64(uint8_t* b, uint64_t v) {
  put32(b, static_cast<uint32_t>(v >> 32));
  put32(b + 4, static_cast<uint32_t>(v));
}
uint32_t get32(const uint8_t* b) {
  return (static_cast<uint32_t>(b[0]) << 24) |
         (static_cast<uint32_t>(b[1]) << 16) |
         (static_cast<uint32_t>(b[2]) << 8) | static_cast<uint32_t>(b[3]);
}
uint64_t get64(const uint8_t* b) {
  return (static_cast<uint64_t>(get32(b)) << 32) | get32(b + 4);
}
bool validEntries(const CandidateAck& a) {
  if (a.count == 0U || a.count > kMaxEntries ||
      (a.flags & static_cast<uint8_t>(~kRelayAllowed)) != 0U) return false;
  for (size_t i = 0; i < a.count; ++i) {
    const Entry& e = a.entries[i];
    if (e.object_len != 84U && e.object_len != 100U) return false;
    if (i != 0U) {
      const Entry& p = a.entries[i - 1U];
      if (e.object_len < p.object_len ||
          (e.object_len == p.object_len && e.object_ref <= p.object_ref))
        return false;
    }
  }
  return true;
}
} // namespace

bool encode(const CandidateAck& a, uint8_t* out, size_t len) {
  if (!out || !validEntries(a) || len != kPrefixSize +
        static_cast<size_t>(a.count) * kEntrySize + kTagSize) return false;
  uint8_t candidate[kMaxSize]{};
  candidate[0] = kVersion;
  candidate[1] = kCandidateType;
  candidate[2] = a.flags;
  candidate[3] = a.count;
  put64(candidate + 4, a.tracker_id);
  put64(candidate + 12, a.gateway_id);
  put32(candidate + 20, a.key_epoch);
  put32(candidate + 24, a.custody_policy_floor);
  put32(candidate + 28, a.custody_grant_generation);
  for (size_t i = 0; i < a.count; ++i) {
    const size_t at = kPrefixSize + i * kEntrySize;
    candidate[at] = a.entries[i].object_len;
    put64(candidate + at + 1U, a.entries[i].object_ref);
  }
  memcpy(candidate + len - kTagSize, a.opaque_mac, kTagSize);
  memcpy(out, candidate, len);
  return true;
}

bool decode(const uint8_t* wire, size_t len, CandidateAck* out) {
  if (!wire || !out || len < kMinSize || len > kMaxSize ||
      wire[0] != kVersion || wire[1] != kCandidateType) return false;
  const uint8_t count = wire[3];
  if (count == 0U || count > kMaxEntries ||
      len != kPrefixSize + static_cast<size_t>(count) * kEntrySize + kTagSize)
    return false;
  CandidateAck candidate;
  candidate.flags = wire[2];
  candidate.count = count;
  candidate.tracker_id = get64(wire + 4);
  candidate.gateway_id = get64(wire + 12);
  candidate.key_epoch = get32(wire + 20);
  candidate.custody_policy_floor = get32(wire + 24);
  candidate.custody_grant_generation = get32(wire + 28);
  for (size_t i = 0; i < count; ++i) {
    const size_t at = kPrefixSize + i * kEntrySize;
    candidate.entries[i].object_len = wire[at];
    candidate.entries[i].object_ref = get64(wire + at + 1U);
  }
  if (!validEntries(candidate)) return false;
  memcpy(candidate.opaque_mac, wire + len - kTagSize, kTagSize);
  *out = candidate;
  return true;
}

} // namespace sf5k1_candidate
} // namespace orun_tlp
