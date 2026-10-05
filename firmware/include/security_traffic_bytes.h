#pragma once

#include <stddef.h>
#include <stdint.h>

namespace orun_tlp {

constexpr uint8_t kSecurityTrafficDirectionD2a = 0x01U;
constexpr uint8_t kSecurityTrafficDirectionA2d = 0x02U;

constexpr size_t kSecurityTrafficLabelSize = 16U;
constexpr size_t kSecurityTrafficInfoSize = 21U;
constexpr size_t kSecurityTrafficNonceSize = 13U;
constexpr size_t kSecurityTrafficKeySize = 16U;
constexpr size_t kSecurityTrafficTagSize = 8U;

inline bool securityTrafficDirectionValid(uint8_t direction) {
  return direction == kSecurityTrafficDirectionD2a ||
         direction == kSecurityTrafficDirectionA2d;
}

inline void writeSecurityBe32(uint32_t value, uint8_t* out) {
  out[0] = static_cast<uint8_t>(value >> 24);
  out[1] = static_cast<uint8_t>(value >> 16);
  out[2] = static_cast<uint8_t>(value >> 8);
  out[3] = static_cast<uint8_t>(value);
}

inline void writeSecurityBe64(uint64_t value, uint8_t* out) {
  for (uint8_t i = 0; i < 8U; ++i)
    out[i] = static_cast<uint8_t>(value >> (56U - 8U * i));
}

inline bool buildSecurityTrafficInfo(
    uint8_t direction, uint32_t key_epoch,
    uint8_t (&out)[kSecurityTrafficInfoSize]) {
  if (!securityTrafficDirectionValid(direction) || key_epoch == UINT32_MAX)
    return false;

  static constexpr uint8_t kLabel[kSecurityTrafficLabelSize] = {
      'O', 'R', 'U', 'N', '-', 'T', 'L', 'P',
      '-', 'V', '2', '-', 'A', 'E', 'A', 'D',
  };
  for (size_t i = 0; i < kSecurityTrafficLabelSize; ++i) out[i] = kLabel[i];
  out[kSecurityTrafficLabelSize] = direction;
  writeSecurityBe32(key_epoch, out + kSecurityTrafficLabelSize + 1U);
  return true;
}

inline bool buildSecurityTrafficNonce(
    uint8_t direction, uint32_t key_epoch, uint64_t counter,
    uint8_t (&out)[kSecurityTrafficNonceSize]) {
  if (!securityTrafficDirectionValid(direction) ||
      key_epoch == UINT32_MAX || counter == 0U)
    return false;

  writeSecurityBe32(key_epoch, out);
  out[4] = direction;
  writeSecurityBe64(counter, out + 5U);
  return true;
}

}  // namespace orun_tlp
