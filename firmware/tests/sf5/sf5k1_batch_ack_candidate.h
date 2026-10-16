#pragma once

// SF5K1 host-only proposed batch ACK *structural* codec.
// UNAPPROVED type 0x06. No MAC verification, receipt authority, routing,
// tracker record release, or firmware runtime linkage is provided.
#include <stddef.h>
#include <stdint.h>

namespace orun_tlp {
namespace sf5k1_candidate {

constexpr uint8_t kVersion = 0x02U;
constexpr uint8_t kCandidateType = 0x06U;
constexpr uint8_t kRelayAllowed = 0x01U;
constexpr size_t kMaxEntries = 4U;
constexpr size_t kPrefixSize = 32U;
constexpr size_t kEntrySize = 9U;
constexpr size_t kTagSize = 16U;
constexpr size_t kMinSize = kPrefixSize + kEntrySize + kTagSize;
constexpr size_t kMaxSize = kPrefixSize + kMaxEntries * kEntrySize + kTagSize;
static_assert(kMinSize == 57U && kMaxSize == 84U, "SF5K proposal size");

struct Entry {
  uint8_t object_len = 0U; // exact 84B EVENT or 100B PERIODIC ciphertext
  uint64_t object_ref = 0U; // SHA256(exact object) first 8 bytes, not authority
};

struct CandidateAck {
  uint8_t flags = 0U;
  uint8_t count = 0U;
  uint64_t tracker_id = 0U;
  uint64_t gateway_id = 0U;
  uint32_t key_epoch = 0U;
  uint32_t custody_policy_floor = 0U;
  uint32_t custody_grant_generation = 0U;
  Entry entries[kMaxEntries]{};
  uint8_t opaque_mac[kTagSize]{}; // NEVER authenticated by this codec
};

// Structural encoding/decoding only; a parsed message is UNTRUSTED.
// Output and destination struct remain unchanged on failure.
// This deliberately exposes no authorize/accept/release operation.
bool encode(const CandidateAck& ack, uint8_t* out, size_t len);
bool decode(const uint8_t* wire, size_t len, CandidateAck* out);

} // namespace sf5k1_candidate
} // namespace orun_tlp
