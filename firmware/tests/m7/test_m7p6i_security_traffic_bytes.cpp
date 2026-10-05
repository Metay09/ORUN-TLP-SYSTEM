#include <assert.h>
#include <stdint.h>
#include <string.h>

#include "security_traffic_bytes.h"
#include "tlp_v2_history_secure.h"

using namespace orun_tlp;

int main() {
  static_assert(kSecurityTrafficDirectionD2a ==
                tlp::kHistoryTrafficDirectionD2a);
  static_assert(kSecurityTrafficDirectionA2d ==
                tlp::kHistoryTrafficDirectionA2d);
  static_assert(kSecurityTrafficInfoSize == 21U);
  static_assert(kSecurityTrafficNonceSize == 13U);
  static_assert(kSecurityTrafficKeySize == 16U);
  static_assert(kSecurityTrafficTagSize == 8U);

  uint8_t info[kSecurityTrafficInfoSize]{};
  assert(buildSecurityTrafficInfo(
      kSecurityTrafficDirectionD2a, 0x01020304U, info));
  const uint8_t expected_d2a_info[kSecurityTrafficInfoSize] = {
      'O','R','U','N','-','T','L','P','-','V','2','-','A','E','A','D',
      0x01,0x01,0x02,0x03,0x04};
  assert(memcmp(info, expected_d2a_info, sizeof(info)) == 0);

  assert(buildSecurityTrafficInfo(
      kSecurityTrafficDirectionA2d, 0x01020304U, info));
  const uint8_t expected_a2d_info[kSecurityTrafficInfoSize] = {
      'O','R','U','N','-','T','L','P','-','V','2','-','A','E','A','D',
      0x02,0x01,0x02,0x03,0x04};
  assert(memcmp(info, expected_a2d_info, sizeof(info)) == 0);

  uint8_t nonce[kSecurityTrafficNonceSize]{};
  assert(buildSecurityTrafficNonce(
      kSecurityTrafficDirectionD2a, 0x01020304U,
      UINT64_C(0x1122334455667788), nonce));
  const uint8_t expected_d2a_nonce[kSecurityTrafficNonceSize] = {
      0x01,0x02,0x03,0x04,0x01,
      0x11,0x22,0x33,0x44,0x55,0x66,0x77,0x88};
  assert(memcmp(nonce, expected_d2a_nonce, sizeof(nonce)) == 0);

  assert(buildSecurityTrafficNonce(
      kSecurityTrafficDirectionA2d, 0x01020304U,
      UINT64_C(0x8877665544332211), nonce));
  const uint8_t expected_a2d_nonce[kSecurityTrafficNonceSize] = {
      0x01,0x02,0x03,0x04,0x02,
      0x88,0x77,0x66,0x55,0x44,0x33,0x22,0x11};
  assert(memcmp(nonce, expected_a2d_nonce, sizeof(nonce)) == 0);

  memset(info, 0xA5, sizeof(info));
  assert(!buildSecurityTrafficInfo(0x00U, 1U, info));
  for (uint8_t byte : info) assert(byte == 0xA5U);
  assert(!buildSecurityTrafficInfo(
      kSecurityTrafficDirectionD2a, UINT32_MAX, info));
  for (uint8_t byte : info) assert(byte == 0xA5U);

  memset(nonce, 0x5A, sizeof(nonce));
  assert(!buildSecurityTrafficNonce(0x03U, 1U, 1U, nonce));
  for (uint8_t byte : nonce) assert(byte == 0x5AU);
  assert(!buildSecurityTrafficNonce(
      kSecurityTrafficDirectionD2a, UINT32_MAX, 1U, nonce));
  for (uint8_t byte : nonce) assert(byte == 0x5AU);
  assert(!buildSecurityTrafficNonce(
      kSecurityTrafficDirectionD2a, 1U, 0U, nonce));
  for (uint8_t byte : nonce) assert(byte == 0x5AU);

  return 0;
}
