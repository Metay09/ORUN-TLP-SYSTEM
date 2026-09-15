#pragma once

#include <stddef.h>
#include <stdint.h>

namespace orun_tlp {

// Opaque logical identity at the core boundary. Existing RAK devices retain
// their exact legacy uint64 value; callers outside compatibility/protocol edges
// should pass DeviceIdentity rather than depend on the board register recipe.
class DeviceIdentity {
 public:
  constexpr DeviceIdentity() = default;

  static constexpr DeviceIdentity fromLegacyUint64(uint64_t value) {
    return DeviceIdentity(value);
  }

  constexpr uint64_t legacyUint64() const { return legacy_value_; }

  friend constexpr bool operator==(DeviceIdentity lhs, DeviceIdentity rhs) {
    return lhs.legacy_value_ == rhs.legacy_value_;
  }
  friend constexpr bool operator!=(DeviceIdentity lhs, DeviceIdentity rhs) {
    return !(lhs == rhs);
  }

 private:
  explicit constexpr DeviceIdentity(uint64_t value) : legacy_value_(value) {}
  uint64_t legacy_value_ = 0;
};

// Decode the exact 8-byte legacy BoardGetUniqueId representation: most
// significant byte first. This helper is driver-free so compatibility can be
// regression-tested on the host without Arduino, Nordic or SX126x headers.
inline bool deviceIdentityFromLegacyBytes(const uint8_t* bytes, size_t size,
                                          DeviceIdentity* identity) {
  if (bytes == nullptr || identity == nullptr || size != 8) return false;
  uint64_t value = 0;
  for (size_t index = 0; index < 8; ++index)
    value = (value << 8) | bytes[index];
  *identity = DeviceIdentity::fromLegacyUint64(value);
  return true;
}

}  // namespace orun_tlp
