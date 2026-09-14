#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "device_identity.h"
#include "gnss_fix.h"
#include "legacy_position_mapping.h"

using namespace orun_tlp;

namespace {

constexpr uint8_t kPositionGolden[] = {
    0x01, 0x02,
    0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF,
    0x91, 0xA2, 0xB3, 0xC4,
    0x65, 0xA1, 0xB2, 0xC3,
    0xEB, 0xCE, 0xD9, 0x88,
    0xD6, 0x34, 0x02, 0x79,
    0xFF, 0xFF, 0xCF, 0xC7,
    0x01, 0x23,
    0x0D, 0x07};

void identityCompatibility() {
  const struct {
    uint8_t bytes[8];
    uint64_t expected;
  } cases[] = {
      {{0x09, 0xA4, 0x62, 0xBD, 0x4B, 0x27, 0x5B, 0xA5},
       0x09A462BD4B275BA5ULL},
      {{0x0E, 0x8A, 0xDE, 0x7E, 0x71, 0x53, 0x1A, 0xA3},
       0x0E8ADE7E71531AA3ULL},
      {{0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01}, 1ULL},
      {{0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}, UINT64_MAX},
  };

  for (const auto& value : cases) {
    DeviceIdentity identity{};
    assert(deviceIdentityFromLegacyBytes(value.bytes, sizeof(value.bytes),
                                         &identity));
    assert(identity.legacyUint64() == value.expected);
  }

  DeviceIdentity unchanged = DeviceIdentity::fromLegacyUint64(0x1122ULL);
  assert(!deviceIdentityFromLegacyBytes(nullptr, 8, &unchanged));
  assert(unchanged.legacyUint64() == 0x1122ULL);
  const uint8_t short_value[7]{};
  assert(!deviceIdentityFromLegacyBytes(short_value, sizeof(short_value),
                                        &unchanged));
  assert(!deviceIdentityFromLegacyBytes(cases[0].bytes, 8, nullptr));
}

void exactLegacyPositionMapping() {
  GnssFix fix{};
  fix.utc_epoch_seconds = 0x65A1B2C3;
  fix.latitude_e7 = -338765432;
  fix.longitude_e7 = -701234567;
  fix.altitude_mm = -12345;
  fix.hdop_x100 = 291;
  fix.satellites = 13;
  fix.flags = 7;
  fix.captured_at_ms = 0xDEADBEEF;  // Local-only metadata must not reach wire.

  uint8_t encoded[sizeof(kPositionGolden)]{};
  assert(encodeLegacyPosition(
      fix, DeviceIdentity::fromLegacyUint64(0x0123456789ABCDEFULL),
      0x91A2B3C4, encoded, sizeof(encoded)));
  assert(memcmp(encoded, kPositionGolden, sizeof(encoded)) == 0);

  assert(!encodeLegacyPosition(
      fix, DeviceIdentity::fromLegacyUint64(0x0123456789ABCDEFULL),
      0x91A2B3C4, nullptr, sizeof(encoded)));
  assert(!encodeLegacyPosition(
      fix, DeviceIdentity::fromLegacyUint64(0x0123456789ABCDEFULL),
      0x91A2B3C4, encoded, sizeof(encoded) - 1));
}

}  // namespace

int main() {
  identityCompatibility();
  exactLegacyPositionMapping();
  puts("B2 portable identity and legacy POSITION mapping: PASS");
}
