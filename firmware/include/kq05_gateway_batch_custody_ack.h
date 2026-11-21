#pragma once

#include <stddef.h>
#include <stdint.h>

namespace orun_tlp {
namespace kq05_gateway_batch_ack {

// KQ05 experimental LoRa Gateway-local CUSTODY acknowledgement envelope.
// Distinct from single-object type 0x05 (56B) and BACKEND durable type 0x09.
// NO authority, MAC verification, signer, radio TX or record release here.
// A single ACK can describe 1..5 byte-exact protected objects, NOT ranges.
// The exact full SHA-256 of EACH object must be bound to a scoped MAC by
// future trusted Gateway and Tracker owners. Prefixes alone are NOT proof.
constexpr uint8_t kVersion = 0x02U;
constexpr uint8_t kType = 0x0AU;
constexpr uint8_t kRelayAllowed = 0x01U;
constexpr uint8_t kMaxObjects = 5U;
constexpr size_t kHeaderSize = 32U;
constexpr size_t kRefSize = 8U;
constexpr size_t kTagSize = 16U;
constexpr size_t kMinSize = kHeaderSize + kRefSize + kTagSize;
constexpr size_t kMaxSize = kHeaderSize + kMaxObjects*kRefSize + kTagSize;

// A different HMAC domain from single-object Gateway custody and Backend
// receipts is mandatory. The HMAC input for the eventual authorized owner:
//   domain (without NUL) || exact transmitted prefix (32+8*N bytes)
//   || concatenation of N FULL SHA-256 digests, sorted by the wire ref order.
// The private K_custody_ack key is already derived for the immutable local
// tracker/gateway/epoch/floor/grant scope by the current grant owner.
// Never derive this key from untrusted header data or reuse a Backend key.
constexpr char kMacDomain[] = "ORUN-KQ05-GW-BATCH-ACK-V1";
constexpr size_t kMaxMacInput =
    sizeof(kMacDomain)-1U + (kMaxSize-kTagSize) + 32U*kMaxObjects;
static_assert(kMaxSize == 88U, "KQ05 batch Gateway wire length changed");
static_assert(kMaxMacInput <= 272U,
              "Gateway batch HMAC exceeds existing CC310 bounded primitive");

struct Header {
  uint64_t tracker_device_id = 0U;
  uint64_t gateway_device_id = 0U;
  uint32_t key_epoch = UINT32_MAX;
  uint32_t custody_floor = UINT32_MAX;
  uint32_t grant_generation = 0U;
  uint8_t count = 0U;
  uint8_t refs[kMaxObjects][kRefSize]{};
};

// Strict structural check only. Output unchanged on failure.
// No caller may interpret true as an authenticated or durable custody ACK.
bool decode(const uint8_t* wire, size_t length, Header& output);

// Serializes a structurally valid candidate with a caller-supplied 16-byte
// MAC; this is NOT an authorized signer. Only an owner that verified all
// N exact objects durably held AND derived the correct scoped key may use
// the resulting bytes as a future authenticated Gateway acknowledgement.
// In particular, this module MUST NOT be called directly by radio RX/TX.
bool encode(const Header& header, const uint8_t (&tag)[kTagSize],
            uint8_t* output, size_t capacity, size_t& written);

} // namespace kq05_gateway_batch_ack
} // namespace orun_tlp
