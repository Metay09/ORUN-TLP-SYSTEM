#include "tlp_v2_delegated_secure_app.h"

#include <string.h>

namespace orun_tlp::tlp {
namespace {

void writeU32BigEndian(uint8_t* output, uint32_t value) {
  output[0] = static_cast<uint8_t>(value >> 24);
  output[1] = static_cast<uint8_t>(value >> 16);
  output[2] = static_cast<uint8_t>(value >> 8);
  output[3] = static_cast<uint8_t>(value);
}

void writeU64BigEndian(uint8_t* output, uint64_t value) {
  for (uint8_t index = 0; index < 8U; ++index) {
    output[index] = static_cast<uint8_t>(value >> (56U - 8U * index));
  }
}

uint32_t readU32BigEndian(const uint8_t* input) {
  return (static_cast<uint32_t>(input[0]) << 24) |
         (static_cast<uint32_t>(input[1]) << 16) |
         (static_cast<uint32_t>(input[2]) << 8) |
         static_cast<uint32_t>(input[3]);
}

uint64_t readU64BigEndian(const uint8_t* input) {
  uint64_t value = 0;
  for (uint8_t index = 0; index < 8U; ++index) {
    value = (value << 8U) | input[index];
  }
  return value;
}

bool securityContextValid(uint8_t security_context) {
  return security_context == kSecurityContextDelegatedGw2d ||
         security_context == kSecurityContextDelegatedD2gw;
}

bool appFamilyValid(uint8_t app_family) {
  return app_family == kDelegatedAppFamilyCommand ||
         app_family == kDelegatedAppFamilyResult;
}

}  // namespace

bool delegatedSecureContextFamilyAllowed(uint8_t security_context,
                                         uint8_t app_family) {
  return (security_context == kSecurityContextDelegatedGw2d &&
          app_family == kDelegatedAppFamilyCommand) ||
         (security_context == kSecurityContextDelegatedD2gw &&
          app_family == kDelegatedAppFamilyResult);
}

bool validateDelegatedSecureAppPacket(const DelegatedSecureAppPacket& packet) {
  return securityContextValid(packet.security_context) &&
         appFamilyValid(packet.app_family) &&
         delegatedSecureContextFamilyAllowed(packet.security_context,
                                              packet.app_family) &&
         (packet.path_flags &
          static_cast<uint8_t>(~kDelegatedPathFlagsAllowedMask)) == 0U &&
         (packet.grant_flags &
          static_cast<uint8_t>(~kDelegatedGrantFlagsAllowedMask)) == 0U &&
         packet.ciphertext_len <= kDelegatedSecureMaxCiphertextSize &&
         packet.key_epoch != UINT32_MAX &&
         packet.security_counter >= 1U &&
         packet.gateway_policy_floor != UINT32_MAX &&
         packet.gateway_grant_generation != 0U &&
         packet.gateway_grant_generation != UINT32_MAX;
}

bool serializeDelegatedSecureAppPacket(const DelegatedSecureAppPacket& packet,
                                       uint8_t* output, size_t output_size) {
  const size_t expected_size =
      kDelegatedSecureHeaderSize + packet.ciphertext_len +
      kDelegatedSecureTagSize;
  if (output == nullptr || output_size != expected_size ||
      !validateDelegatedSecureAppPacket(packet)) {
    return false;
  }

  output[0] = kProtocolVersionV2;
  output[1] = kPacketTypeDelegatedSecureApp;
  output[2] = packet.security_context;
  output[3] = packet.app_family;
  output[4] = packet.path_flags;
  output[5] = packet.grant_flags;
  output[6] = packet.ciphertext_len;
  output[7] = 0U;

  writeU64BigEndian(&output[8], packet.origin_device_id);
  writeU64BigEndian(&output[16], packet.target_device_id);
  writeU32BigEndian(&output[24], packet.key_epoch);
  writeU64BigEndian(&output[28], packet.security_counter);
  writeU32BigEndian(&output[36], packet.gateway_policy_floor);
  writeU32BigEndian(&output[40], packet.gateway_grant_generation);
  memcpy(&output[44], packet.frame_key_salt, kDelegatedSecureFrameSaltSize);
  memcpy(&output[kDelegatedSecureHeaderSize], packet.ciphertext,
         packet.ciphertext_len);
  memcpy(&output[kDelegatedSecureHeaderSize + packet.ciphertext_len],
         packet.tag, kDelegatedSecureTagSize);
  return true;
}

DelegatedSecureDecodeStatus deserializeDelegatedSecureAppPacket(
    const uint8_t* input, size_t input_size, DelegatedSecureAppPacket* packet) {
  if (input == nullptr || packet == nullptr ||
      input_size < kDelegatedSecureMinPacketSize ||
      input_size > kDelegatedSecureMaxPacketSize) {
    return DelegatedSecureDecodeStatus::kLength;
  }
  if (input[0] != kProtocolVersionV2)
    return DelegatedSecureDecodeStatus::kVersion;
  if (input[1] != kPacketTypeDelegatedSecureApp)
    return DelegatedSecureDecodeStatus::kType;
  if (!securityContextValid(input[2]))
    return DelegatedSecureDecodeStatus::kSecurityContext;
  if (!appFamilyValid(input[3]))
    return DelegatedSecureDecodeStatus::kAppFamily;
  if (!delegatedSecureContextFamilyAllowed(input[2], input[3]))
    return DelegatedSecureDecodeStatus::kContextFamily;
  if ((input[4] & static_cast<uint8_t>(~kDelegatedPathFlagsAllowedMask)) != 0U)
    return DelegatedSecureDecodeStatus::kPathFlags;
  if ((input[5] & static_cast<uint8_t>(~kDelegatedGrantFlagsAllowedMask)) != 0U)
    return DelegatedSecureDecodeStatus::kGrantFlags;
  if (input[6] > kDelegatedSecureMaxCiphertextSize)
    return DelegatedSecureDecodeStatus::kCiphertextLength;
  if (input[7] != 0U)
    return DelegatedSecureDecodeStatus::kReserved;

  const size_t expected_size =
      kDelegatedSecureHeaderSize + input[6] + kDelegatedSecureTagSize;
  if (input_size != expected_size)
    return DelegatedSecureDecodeStatus::kLength;

  const uint32_t key_epoch = readU32BigEndian(&input[24]);
  const uint64_t security_counter = readU64BigEndian(&input[28]);
  const uint32_t policy_floor = readU32BigEndian(&input[36]);
  const uint32_t grant_generation = readU32BigEndian(&input[40]);

  if (key_epoch == UINT32_MAX)
    return DelegatedSecureDecodeStatus::kKeyEpoch;
  if (security_counter == 0U)
    return DelegatedSecureDecodeStatus::kSecurityCounter;
  if (policy_floor == UINT32_MAX)
    return DelegatedSecureDecodeStatus::kPolicyFloor;
  if (grant_generation == 0U || grant_generation == UINT32_MAX)
    return DelegatedSecureDecodeStatus::kGrantGeneration;

  DelegatedSecureAppPacket decoded{};
  decoded.security_context = input[2];
  decoded.app_family = input[3];
  decoded.path_flags = input[4];
  decoded.grant_flags = input[5];
  decoded.ciphertext_len = input[6];
  decoded.origin_device_id = readU64BigEndian(&input[8]);
  decoded.target_device_id = readU64BigEndian(&input[16]);
  decoded.key_epoch = key_epoch;
  decoded.security_counter = security_counter;
  decoded.gateway_policy_floor = policy_floor;
  decoded.gateway_grant_generation = grant_generation;
  memcpy(decoded.frame_key_salt, &input[44], kDelegatedSecureFrameSaltSize);
  memcpy(decoded.ciphertext, &input[kDelegatedSecureHeaderSize],
         decoded.ciphertext_len);
  memcpy(decoded.tag,
         &input[kDelegatedSecureHeaderSize + decoded.ciphertext_len],
         kDelegatedSecureTagSize);

  *packet = decoded;
  return DelegatedSecureDecodeStatus::kOk;
}

}  // namespace orun_tlp::tlp
