#include "custody_ack.h"

#include "tlp_v2_product_secure.h"

namespace orun_tlp {
namespace custody_ack {
namespace {

uint32_t readBe32(const uint8_t* bytes) {
  return (uint32_t(bytes[0]) << 24) |
         (uint32_t(bytes[1]) << 16) |
         (uint32_t(bytes[2]) << 8) | uint32_t(bytes[3]);
}

uint64_t readBe64(const uint8_t* bytes) {
  uint64_t result = 0U;
  for (unsigned i = 0U; i < 8U; ++i)
    result = (result << 8) | uint64_t(bytes[i]);
  return result;
}

}  // namespace

bool decodeHeader(const uint8_t* bytes, size_t size, Header& out) {
  if (bytes == nullptr || size != kWireSize ||
      bytes[0] != kVersion || bytes[1] != kType ||
      (bytes[2] & static_cast<uint8_t>(~kRelayAllowed)) != 0U ||
      (bytes[3] != tlp::kEventFrameSize &&
       bytes[3] != tlp::kPeriodicFrameSize))
    return false;

  Header parsed{};
  parsed.relay_allowed = (bytes[2] & kRelayAllowed) != 0U;
  parsed.object_len = bytes[3];
  parsed.tracker_device_id = readBe64(bytes + 4U);
  parsed.gateway_device_id = readBe64(bytes + 12U);
  parsed.key_epoch = readBe32(bytes + 20U);
  parsed.custody_floor = readBe32(bytes + 24U);
  parsed.grant_generation = readBe32(bytes + 28U);
  for (size_t i = 0U; i < kObjectRefSize; ++i)
    parsed.object_ref[i] = bytes[32U + i];

  if (parsed.tracker_device_id == 0U ||
      parsed.gateway_device_id == 0U ||
      parsed.key_epoch == UINT32_MAX ||
      parsed.custody_floor == UINT32_MAX ||
      parsed.grant_generation == 0U ||
      parsed.grant_generation == UINT32_MAX)
    return false;
  out = parsed;
  return true;
}

bool matchesProtectedShape(const Header& ack, const uint8_t* object,
                           size_t size) {
  if (object == nullptr || size != ack.object_len ||
      (size != tlp::kEventFrameSize &&
       size != tlp::kPeriodicFrameSize))
    return false;

  tlp::ProductSecureFrame frame{};
  return tlp::decodeProductSecureFrame(object, size, &frame) ==
             tlp::ProductSecureCodecResult::kOk &&
         frame.device_id == ack.tracker_device_id &&
         frame.key_epoch == ack.key_epoch &&
         (frame.path_flags & tlp::kProductPathFlagCustodyRequested) != 0U;
}

}  // namespace custody_ack
}  // namespace orun_tlp
