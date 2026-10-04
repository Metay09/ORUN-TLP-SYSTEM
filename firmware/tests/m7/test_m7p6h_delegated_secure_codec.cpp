#include <assert.h>
#include <stdint.h>
#include <string.h>

#include "tlp_v2_delegated_secure_app.h"

namespace {

using orun_tlp::tlp::DelegatedSecureAppPacket;
using orun_tlp::tlp::DelegatedSecureDecodeStatus;

constexpr uint8_t grantFlags(uint8_t scope_id, uint8_t quota_code) {
  return static_cast<uint8_t>((scope_id << 2U) | quota_code);
}

DelegatedSecureAppPacket makePacket() {
  using namespace orun_tlp::tlp;
  DelegatedSecureAppPacket packet{};
  packet.security_context = kSecurityContextDelegatedGw2d;
  packet.app_family = kDelegatedAppFamilyCommand;
  packet.path_flags = kDelegatedPathFlagRelayAllowed;
  packet.grant_flags = grantFlags(5U, 2U);
  packet.ciphertext_len = kDelegatedSecureMaxCiphertextSize;
  packet.origin_device_id = UINT64_C(0x1122334455667788);
  packet.target_device_id = UINT64_C(0x8877665544332211);
  packet.key_epoch = 0x01020304U;
  packet.security_counter = UINT64_C(0x0102030405060708);
  packet.gateway_policy_floor = 0x0A0B0C0DU;
  packet.gateway_grant_generation = 0x10203040U;

  for (uint8_t i = 0; i < kDelegatedSecureFrameSaltSize; ++i) {
    packet.frame_key_salt[i] = static_cast<uint8_t>(0xD0U + i);
  }
  for (uint8_t i = 0; i < kDelegatedSecureMaxCiphertextSize; ++i) {
    packet.ciphertext[i] = static_cast<uint8_t>(0xA0U + i);
  }
  for (uint8_t i = 0; i < kDelegatedSecureTagSize; ++i) {
    packet.tag[i] = static_cast<uint8_t>(0xC0U + i);
  }
  return packet;
}

void expectDecodeStatus(const uint8_t* frame, size_t size,
                        DelegatedSecureDecodeStatus expected) {
  DelegatedSecureAppPacket decoded{};
  assert(orun_tlp::tlp::deserializeDelegatedSecureAppPacket(
             frame, size, &decoded) == expected);
}

}  // namespace

int main() {
  using namespace orun_tlp::tlp;

  static_assert(kDelegatedSecureHeaderSize == 56U);
  static_assert(kDelegatedSecureMaxCiphertextSize == 32U);
  static_assert(kDelegatedSecureTagSize == 8U);
  static_assert(kDelegatedSecureMinPacketSize == 64U);
  static_assert(kDelegatedSecureMaxPacketSize == 96U);

  DelegatedSecureAppPacket packet = makePacket();
  assert(validateDelegatedSecureAppPacket(packet));

  uint8_t encoded[kDelegatedSecureMaxPacketSize]{};
  assert(serializeDelegatedSecureAppPacket(packet, encoded, sizeof(encoded)));

  static const uint8_t kGolden[kDelegatedSecureMaxPacketSize] = {
      0x02, 0x01, 0x03, 0x01, 0x01, 0x16, 0x20, 0x00,
      0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88,
      0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11,
      0x01, 0x02, 0x03, 0x04,
      0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
      0x0A, 0x0B, 0x0C, 0x0D,
      0x10, 0x20, 0x30, 0x40,
      0xD0, 0xD1, 0xD2, 0xD3, 0xD4, 0xD5,
      0xD6, 0xD7, 0xD8, 0xD9, 0xDA, 0xDB,
      0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7,
      0xA8, 0xA9, 0xAA, 0xAB, 0xAC, 0xAD, 0xAE, 0xAF,
      0xB0, 0xB1, 0xB2, 0xB3, 0xB4, 0xB5, 0xB6, 0xB7,
      0xB8, 0xB9, 0xBA, 0xBB, 0xBC, 0xBD, 0xBE, 0xBF,
      0xC0, 0xC1, 0xC2, 0xC3, 0xC4, 0xC5, 0xC6, 0xC7,
  };
  assert(memcmp(encoded, kGolden, sizeof(kGolden)) == 0);

  DelegatedSecureAppPacket decoded{};
  assert(deserializeDelegatedSecureAppPacket(
             encoded, sizeof(encoded), &decoded) ==
         DelegatedSecureDecodeStatus::kOk);
  assert(decoded.security_context == packet.security_context);
  assert(decoded.app_family == packet.app_family);
  assert(decoded.path_flags == packet.path_flags);
  assert(decoded.grant_flags == packet.grant_flags);
  assert(decoded.ciphertext_len == packet.ciphertext_len);
  assert(decoded.origin_device_id == packet.origin_device_id);
  assert(decoded.target_device_id == packet.target_device_id);
  assert(decoded.key_epoch == packet.key_epoch);
  assert(decoded.security_counter == packet.security_counter);
  assert(decoded.gateway_policy_floor == packet.gateway_policy_floor);
  assert(decoded.gateway_grant_generation ==
         packet.gateway_grant_generation);
  assert(memcmp(decoded.frame_key_salt, packet.frame_key_salt,
                kDelegatedSecureFrameSaltSize) == 0);
  assert(memcmp(decoded.ciphertext, packet.ciphertext,
                packet.ciphertext_len) == 0);
  assert(memcmp(decoded.tag, packet.tag, kDelegatedSecureTagSize) == 0);

  // Envelope-only rule: family-specific plaintext minimums remain with the
  // later COMMAND/RESULT plaintext contract, not this ciphertext container.
  DelegatedSecureAppPacket empty = makePacket();
  empty.ciphertext_len = 0U;
  uint8_t min_frame[kDelegatedSecureMinPacketSize]{};
  assert(serializeDelegatedSecureAppPacket(empty, min_frame,
                                           sizeof(min_frame)));
  assert(deserializeDelegatedSecureAppPacket(min_frame, sizeof(min_frame),
                                              &decoded) ==
         DelegatedSecureDecodeStatus::kOk);

  uint8_t mutated[kDelegatedSecureMaxPacketSize]{};

  memcpy(mutated, kGolden, sizeof(mutated));
  mutated[0] = 0x01U;
  expectDecodeStatus(mutated, sizeof(mutated),
                     DelegatedSecureDecodeStatus::kVersion);

  memcpy(mutated, kGolden, sizeof(mutated));
  mutated[1] = 0x02U;
  expectDecodeStatus(mutated, sizeof(mutated),
                     DelegatedSecureDecodeStatus::kType);

  memcpy(mutated, kGolden, sizeof(mutated));
  mutated[2] = 0x01U;
  expectDecodeStatus(mutated, sizeof(mutated),
                     DelegatedSecureDecodeStatus::kSecurityContext);

  memcpy(mutated, kGolden, sizeof(mutated));
  mutated[3] = 0x7FU;
  expectDecodeStatus(mutated, sizeof(mutated),
                     DelegatedSecureDecodeStatus::kAppFamily);

  memcpy(mutated, kGolden, sizeof(mutated));
  mutated[3] = kDelegatedAppFamilyResult;
  expectDecodeStatus(mutated, sizeof(mutated),
                     DelegatedSecureDecodeStatus::kContextFamily);

  memcpy(mutated, kGolden, sizeof(mutated));
  mutated[4] = 0x02U;
  expectDecodeStatus(mutated, sizeof(mutated),
                     DelegatedSecureDecodeStatus::kPathFlags);

  memcpy(mutated, kGolden, sizeof(mutated));
  mutated[5] = 0x80U;
  expectDecodeStatus(mutated, sizeof(mutated),
                     DelegatedSecureDecodeStatus::kGrantFlags);

  memcpy(mutated, kGolden, sizeof(mutated));
  mutated[6] = 33U;
  expectDecodeStatus(mutated, sizeof(mutated),
                     DelegatedSecureDecodeStatus::kCiphertextLength);

  memcpy(mutated, kGolden, sizeof(mutated));
  mutated[7] = 1U;
  expectDecodeStatus(mutated, sizeof(mutated),
                     DelegatedSecureDecodeStatus::kReserved);

  expectDecodeStatus(kGolden, sizeof(kGolden) - 1U,
                     DelegatedSecureDecodeStatus::kLength);
  expectDecodeStatus(kGolden, kDelegatedSecureMinPacketSize - 1U,
                     DelegatedSecureDecodeStatus::kLength);

  memcpy(mutated, kGolden, sizeof(mutated));
  memset(&mutated[24], 0xFF, 4U);
  expectDecodeStatus(mutated, sizeof(mutated),
                     DelegatedSecureDecodeStatus::kKeyEpoch);

  memcpy(mutated, kGolden, sizeof(mutated));
  memset(&mutated[28], 0x00, 8U);
  expectDecodeStatus(mutated, sizeof(mutated),
                     DelegatedSecureDecodeStatus::kSecurityCounter);

  memcpy(mutated, kGolden, sizeof(mutated));
  memset(&mutated[36], 0xFF, 4U);
  expectDecodeStatus(mutated, sizeof(mutated),
                     DelegatedSecureDecodeStatus::kPolicyFloor);

  memcpy(mutated, kGolden, sizeof(mutated));
  memset(&mutated[40], 0x00, 4U);
  expectDecodeStatus(mutated, sizeof(mutated),
                     DelegatedSecureDecodeStatus::kGrantGeneration);

  memcpy(mutated, kGolden, sizeof(mutated));
  memset(&mutated[40], 0xFF, 4U);
  expectDecodeStatus(mutated, sizeof(mutated),
                     DelegatedSecureDecodeStatus::kGrantGeneration);

  DelegatedSecureAppPacket result = makePacket();
  result.security_context = kSecurityContextDelegatedD2gw;
  result.app_family = kDelegatedAppFamilyResult;
  assert(validateDelegatedSecureAppPacket(result));
  assert(serializeDelegatedSecureAppPacket(result, encoded, sizeof(encoded)));
  assert(deserializeDelegatedSecureAppPacket(encoded, sizeof(encoded),
                                              &decoded) ==
         DelegatedSecureDecodeStatus::kOk);

  DelegatedSecureAppPacket invalid = makePacket();
  invalid.security_counter = 0U;
  assert(!serializeDelegatedSecureAppPacket(invalid, encoded, sizeof(encoded)));

  invalid = makePacket();
  invalid.gateway_grant_generation = 0U;
  assert(!serializeDelegatedSecureAppPacket(invalid, encoded, sizeof(encoded)));

  invalid = makePacket();
  invalid.path_flags = 0x80U;
  assert(!serializeDelegatedSecureAppPacket(invalid, encoded, sizeof(encoded)));

  invalid = makePacket();
  invalid.app_family = kDelegatedAppFamilyResult;
  assert(!serializeDelegatedSecureAppPacket(invalid, encoded, sizeof(encoded)));

  return 0;
}
