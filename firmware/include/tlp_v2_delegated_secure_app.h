#pragma once

#include <stddef.h>
#include <stdint.h>

namespace orun_tlp::tlp {

constexpr uint8_t kProtocolVersionV2 = 0x02U;
constexpr uint8_t kPacketTypeDelegatedSecureApp = 0x01U;

constexpr uint8_t kSecurityContextDelegatedGw2d = 0x03U;
constexpr uint8_t kSecurityContextDelegatedD2gw = 0x04U;

constexpr uint8_t kDelegatedAppFamilyCommand = 0x01U;
constexpr uint8_t kDelegatedAppFamilyResult = 0x02U;

constexpr uint8_t kDelegatedPathFlagRelayAllowed = 1U << 0;
constexpr uint8_t kDelegatedPathFlagsAllowedMask =
    kDelegatedPathFlagRelayAllowed;

constexpr uint8_t kDelegatedGrantQuotaMask = 0x03U;
constexpr uint8_t kDelegatedGrantScopeMask = 0x3CU;
constexpr uint8_t kDelegatedGrantScopeShift = 2U;
constexpr uint8_t kDelegatedGrantFlagsAllowedMask =
    kDelegatedGrantQuotaMask | kDelegatedGrantScopeMask;

constexpr size_t kDelegatedSecureHeaderSize = 56U;
constexpr size_t kDelegatedSecureMaxCiphertextSize = 32U;
constexpr size_t kDelegatedSecureTagSize = 8U;
constexpr size_t kDelegatedSecureFrameSaltSize = 12U;
constexpr size_t kDelegatedSecureMinPacketSize =
    kDelegatedSecureHeaderSize + kDelegatedSecureTagSize;
constexpr size_t kDelegatedSecureMaxPacketSize =
    kDelegatedSecureHeaderSize + kDelegatedSecureMaxCiphertextSize +
    kDelegatedSecureTagSize;

struct DelegatedSecureAppPacket {
  uint8_t security_context = 0;
  uint8_t app_family = 0;
  uint8_t path_flags = 0;
  uint8_t grant_flags = 0;
  uint8_t ciphertext_len = 0;

  uint64_t origin_device_id = 0;
  uint64_t target_device_id = 0;
  uint32_t key_epoch = 0;
  uint64_t security_counter = 0;
  uint32_t gateway_policy_floor = 0;
  uint32_t gateway_grant_generation = 0;
  uint8_t frame_key_salt[kDelegatedSecureFrameSaltSize]{};

  uint8_t ciphertext[kDelegatedSecureMaxCiphertextSize]{};
  uint8_t tag[kDelegatedSecureTagSize]{};
};

enum class DelegatedSecureDecodeStatus : uint8_t {
  kOk,
  kLength,
  kVersion,
  kType,
  kSecurityContext,
  kAppFamily,
  kContextFamily,
  kPathFlags,
  kGrantFlags,
  kCiphertextLength,
  kReserved,
  kKeyEpoch,
  kSecurityCounter,
  kPolicyFloor,
  kGrantGeneration,
};

bool delegatedSecureContextFamilyAllowed(uint8_t security_context,
                                         uint8_t app_family);

bool validateDelegatedSecureAppPacket(const DelegatedSecureAppPacket& packet);

bool serializeDelegatedSecureAppPacket(const DelegatedSecureAppPacket& packet,
                                       uint8_t* output, size_t output_size);

DelegatedSecureDecodeStatus deserializeDelegatedSecureAppPacket(
    const uint8_t* input, size_t input_size, DelegatedSecureAppPacket* packet);

}  // namespace orun_tlp::tlp
