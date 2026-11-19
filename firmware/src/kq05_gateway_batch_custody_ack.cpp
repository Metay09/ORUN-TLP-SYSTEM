#include "kq05_gateway_batch_custody_ack.h"

#include <string.h>

namespace orun_tlp {
namespace kq05_gateway_batch_ack {
namespace {
uint64_t be64(const uint8_t* p) {
  uint64_t result = 0U;
  for (size_t i = 0U; i < 8U; ++i) result = (result << 8U) | p[i];
  return result;
}
uint32_t be32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24U) | (uint32_t(p[1]) << 16U) |
         (uint32_t(p[2]) << 8U) | uint32_t(p[3]);
}
void put64(uint8_t* p, uint64_t n) {
  for (size_t i = 0U; i < 8U; ++i)
    p[i] = static_cast<uint8_t>(n >> (56U-8U*i));
}
void put32(uint8_t* p, uint32_t n) {
  p[0] = static_cast<uint8_t>(n >> 24U);
  p[1] = static_cast<uint8_t>(n >> 16U);
  p[2] = static_cast<uint8_t>(n >> 8U);
  p[3] = static_cast<uint8_t>(n);
}
bool valid(const Header& h) {
  if (h.tracker_device_id == 0U || h.tracker_device_id == UINT64_MAX ||
      h.gateway_device_id == 0U || h.gateway_device_id == UINT64_MAX ||
      h.key_epoch == UINT32_MAX || h.custody_floor == UINT32_MAX ||
      h.grant_generation == 0U || h.grant_generation == UINT32_MAX ||
      h.count == 0U || h.count > kMaxObjects)
    return false;
  // Prefixes are hints only; strict ordering ensures deterministic HMAC
  // digest order and makes duplicate/ambiguous references inadmissible.
  // The actual authenticated verifier must reject any ambiguous full digest.
  for (uint8_t i = 1U; i < h.count; ++i) {
    if (memcmp(h.refs[i-1U], h.refs[i], kRefSize) >= 0) return false;
  }
  // Unused ref slots must be zero, avoiding hidden data surviving across
  // decoding/reuse and canonicalizing the output before any MAC calculation.
  for (uint8_t i = h.count; i < kMaxObjects; ++i)
    for (uint8_t j = 0U; j < kRefSize; ++j)
      if (h.refs[i][j] != 0U) return false;
  return true;
}
} // namespace

bool decode(const uint8_t* wire, size_t length, Header& output) {
  if (wire == nullptr || length < kMinSize || length > kMaxSize ||
      wire[0] != kVersion || wire[1] != kType ||
      wire[2] != kRelayAllowed || wire[3] < 1U ||
      wire[3] > kMaxObjects ||
      length != kHeaderSize + kRefSize * wire[3] + kTagSize)
    return false;

  Header candidate{};
  candidate.count = wire[3];
  candidate.tracker_device_id = be64(wire + 4U);
  candidate.gateway_device_id = be64(wire + 12U);
  candidate.key_epoch = be32(wire + 20U);
  candidate.custody_floor = be32(wire + 24U);
  candidate.grant_generation = be32(wire + 28U);
  for (uint8_t i = 0U; i < candidate.count; ++i)
    memcpy(candidate.refs[i], wire + kHeaderSize + kRefSize*i, kRefSize);
  if (!valid(candidate)) return false;
  output = candidate;
  return true;
}

bool encode(const Header& header, const uint8_t (&tag)[kTagSize],
            uint8_t* output, size_t capacity, size_t& written) {
  if (!valid(header) || output == nullptr) return false;
  const size_t length = kHeaderSize + kRefSize * header.count + kTagSize;
  if (capacity < length) return false;
  uint8_t bytes[kMaxSize]{};
  bytes[0] = kVersion;
  bytes[1] = kType;
  bytes[2] = kRelayAllowed;
  bytes[3] = header.count;
  put64(bytes + 4U, header.tracker_device_id);
  put64(bytes + 12U, header.gateway_device_id);
  put32(bytes + 20U, header.key_epoch);
  put32(bytes + 24U, header.custody_floor);
  put32(bytes + 28U, header.grant_generation);
  for (uint8_t i = 0U; i < header.count; ++i)
    memcpy(bytes + kHeaderSize + kRefSize*i,
           header.refs[i], kRefSize);
  memcpy(bytes + length - kTagSize, tag, kTagSize);
  memcpy(output, bytes, length);
  written = length;
  return true;
}

} // namespace kq05_gateway_batch_ack
} // namespace orun_tlp
