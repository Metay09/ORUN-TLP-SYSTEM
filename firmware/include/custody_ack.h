#pragma once
#include <stddef.h>
#include <stdint.h>

namespace orun_tlp {
namespace custody_ack {

// SF5B section 12: a 56-byte UNTRUSTED Gateway-custody ACK wire.
// These parsers and structural checks do not authenticate a Gateway,
// derive any key or confer release authority.
constexpr uint8_t kVersion = 0x02U;
constexpr uint8_t kType = 0x05U;
constexpr size_t kWireSize = 56U;
constexpr size_t kHeaderSize = 40U;
constexpr size_t kTagSize = 16U;
constexpr size_t kObjectRefSize = 8U;
constexpr uint8_t kRelayAllowed = 0x01U;

struct Header {
  bool relay_allowed = false;
  uint8_t object_len = 0U;
  uint64_t tracker_device_id = 0U;
  uint64_t gateway_device_id = 0U;
  uint32_t key_epoch = UINT32_MAX;
  uint32_t custody_floor = UINT32_MAX;
  uint32_t grant_generation = 0U;
  uint8_t object_ref[kObjectRefSize]{};
};

// Strict wire decoder. The output is unchanged on rejection. An ACK
// is never security authority until independent local durable scope and
// full SHA-256/HKDF/HMAC checks succeed in a separate credential owner.
bool decodeHeader(const uint8_t* bytes, size_t size, Header& out);

// Byte-exact validity for a retained PRODUCT_SECURE custody object.
// The visible frame is opaque and NOT AEAD-authenticated here.
// Return false if the header does not bind one exact local frame length.
// The caller must independently compute SHA256 and verify an HMAC.
bool matchesProtectedShape(const Header& ack, const uint8_t* object,
                           size_t size);

}  // namespace custody_ack
}  // namespace orun_tlp
