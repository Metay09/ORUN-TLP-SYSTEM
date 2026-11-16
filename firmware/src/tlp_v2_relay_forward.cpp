#include "tlp_v2_relay_forward.h"

#include <string.h>

#include "custody_ack.h"
#include "tlp_v2_delegated_secure_app.h"

namespace orun_tlp {
namespace tlp {
namespace {

void put64(uint8_t* out, uint64_t value) {
  for (size_t i = 0; i < 8U; ++i)
    out[i] = static_cast<uint8_t>(value >> (56U - i * 8U));
}

uint64_t get64(const uint8_t* bytes) {
  uint64_t value = 0U;
  for (size_t i = 0; i < 8U; ++i) value = (value << 8U) | bytes[i];
  return value;
}

}  // namespace

V2RelayCodecStatus validateV2RelayInner(const uint8_t* bytes, size_t length) {
  if (bytes == nullptr || length < 2U ||
      length > kV2RelayForwardMaxInnerSize)
    return V2RelayCodecStatus::kLength;
  if (bytes[0] != kV2RelayForwardVersion)
    return V2RelayCodecStatus::kInnerInvalid;
  switch (bytes[1]) {
    case kPacketTypeDelegatedSecureApp: {
      DelegatedSecureAppPacket packet{};
      if (deserializeDelegatedSecureAppPacket(bytes, length, &packet) !=
          DelegatedSecureDecodeStatus::kOk)
        return V2RelayCodecStatus::kInnerInvalid;
      return (packet.path_flags & kDelegatedPathFlagRelayAllowed) != 0U
                 ? V2RelayCodecStatus::kOk
                 : V2RelayCodecStatus::kRelayNotAllowed;
    }
    case kPacketTypeProductSecure: {
      ProductSecureFrame packet{};
      if (decodeProductSecureFrame(bytes, length, &packet) !=
          ProductSecureCodecResult::kOk)
        return V2RelayCodecStatus::kInnerInvalid;
      return (packet.path_flags & kProductPathFlagRelayAllowed) != 0U
                 ? V2RelayCodecStatus::kOk
                 : V2RelayCodecStatus::kRelayNotAllowed;
    }
    case custody_ack::kType: {
      custody_ack::Header header{};
      if (!custody_ack::decodeHeader(bytes, length, header))
        return V2RelayCodecStatus::kInnerInvalid;
      return header.relay_allowed ? V2RelayCodecStatus::kOk
                                  : V2RelayCodecStatus::kRelayNotAllowed;
    }
    default:
      // Includes nested V2_RELAY_FORWARD and legacy HISTORY_SECURE.
      return V2RelayCodecStatus::kInnerType;
  }
}

bool encodeV2RelayForward(const V2RelayForward& packet, uint8_t* output,
                          size_t capacity, size_t& output_length) {
  const size_t length = kV2RelayForwardHeaderSize + packet.inner_length;
  if (output == nullptr || packet.relay_device_id == 0U ||
      length > capacity ||
      validateV2RelayInner(packet.inner, packet.inner_length) !=
          V2RelayCodecStatus::kOk)
    return false;

  output[0] = kV2RelayForwardVersion;
  output[1] = kV2RelayForwardType;
  output[2] = kV2RelayHopCount;
  output[3] = packet.inner_length;
  put64(output + 4U, packet.relay_device_id);
  output[12] =
      static_cast<uint8_t>(static_cast<uint16_t>(packet.ingress_rssi_dbm) >> 8U);
  output[13] = static_cast<uint8_t>(packet.ingress_rssi_dbm);
  output[14] = static_cast<uint8_t>(packet.ingress_snr_db);
  output[15] = 0U;
  memcpy(output + kV2RelayForwardHeaderSize, packet.inner,
         packet.inner_length);
  output_length = length;
  return true;
}

V2RelayCodecStatus decodeV2RelayForward(
    const uint8_t* bytes, size_t length, V2RelayForward& output) {
  if (bytes == nullptr || length < kV2RelayForwardHeaderSize ||
      length > kV2RelayForwardMaxSize)
    return V2RelayCodecStatus::kLength;
  if (bytes[0] != kV2RelayForwardVersion)
    return V2RelayCodecStatus::kVersion;
  if (bytes[1] != kV2RelayForwardType)
    return V2RelayCodecStatus::kType;
  if (bytes[2] != kV2RelayHopCount)
    return V2RelayCodecStatus::kHopCount;
  if (bytes[3] == 0U ||
      length != kV2RelayForwardHeaderSize + bytes[3])
    return V2RelayCodecStatus::kLength;
  if (bytes[15] != 0U || get64(bytes + 4U) == 0U)
    return V2RelayCodecStatus::kReserved;

  const auto status =
      validateV2RelayInner(bytes + kV2RelayForwardHeaderSize, bytes[3]);
  if (status != V2RelayCodecStatus::kOk) return status;

  V2RelayForward candidate{};
  candidate.relay_device_id = get64(bytes + 4U);
  candidate.ingress_rssi_dbm =
      static_cast<int16_t>((uint16_t(bytes[12]) << 8U) | bytes[13]);
  candidate.ingress_snr_db = static_cast<int8_t>(bytes[14]);
  candidate.inner_length = bytes[3];
  memcpy(candidate.inner, bytes + kV2RelayForwardHeaderSize, bytes[3]);
  output = candidate;
  return V2RelayCodecStatus::kOk;
}

}  // namespace tlp
}  // namespace orun_tlp
