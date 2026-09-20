#include <assert.h>
#include <stdint.h>
#include <string.h>

#include "m7p6e_candidate_contract.h"

int main() {
  using namespace orun_tlp::m7p6e_test;

  static_assert(kLabelSize == 16U, "KDF label length changed");
  static_assert(kInfoSize == 21U, "KDF info length changed");
  static_assert(kNonceSize == 13U, "CCM nonce length changed");
  static_assert(kTagSize == 8U, "CCM tag length changed");

  uint8_t d2a_info[kInfoSize]{};
  uint8_t a2d_info[kInfoSize]{};
  buildInfo(kDirectionD2A, d2a_info);
  buildInfo(kDirectionA2D, a2d_info);

  static const uint8_t expected_d2a_info[kInfoSize] = {
      0x4F, 0x52, 0x55, 0x4E, 0x2D, 0x54, 0x4C, 0x50,
      0x2D, 0x56, 0x32, 0x2D, 0x41, 0x45, 0x41, 0x44,
      0x01, 0x01, 0x02, 0x03, 0x04,
  };
  static const uint8_t expected_a2d_info[kInfoSize] = {
      0x4F, 0x52, 0x55, 0x4E, 0x2D, 0x54, 0x4C, 0x50,
      0x2D, 0x56, 0x32, 0x2D, 0x41, 0x45, 0x41, 0x44,
      0x02, 0x01, 0x02, 0x03, 0x04,
  };
  assert(memcmp(d2a_info, expected_d2a_info, sizeof(d2a_info)) == 0);
  assert(memcmp(a2d_info, expected_a2d_info, sizeof(a2d_info)) == 0);
  assert(memcmp(d2a_info, a2d_info, sizeof(d2a_info)) != 0);

  uint8_t d2a_nonce[kNonceSize]{};
  uint8_t a2d_nonce[kNonceSize]{};
  buildNonce(kDirectionD2A, kTxCounter, d2a_nonce);
  buildNonce(kDirectionA2D, kTxCounter, a2d_nonce);

  static const uint8_t expected_d2a_nonce[kNonceSize] = {
      0x01, 0x02, 0x03, 0x04, 0x01,
      0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88,
  };
  static const uint8_t expected_a2d_nonce[kNonceSize] = {
      0x01, 0x02, 0x03, 0x04, 0x02,
      0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88,
  };
  assert(memcmp(d2a_nonce, expected_d2a_nonce, sizeof(d2a_nonce)) == 0);
  assert(memcmp(a2d_nonce, expected_a2d_nonce, sizeof(a2d_nonce)) == 0);
  assert(memcmp(d2a_nonce, a2d_nonce, sizeof(d2a_nonce)) != 0);

  assert(memcmp(kExpectedD2AKey, kExpectedA2DKey, kTrafficKeySize) != 0);

  return 0;
}
