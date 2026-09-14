#include "rak_device_identity.h"

#include <SX126x-Arduino.h>

namespace orun_tlp {

DeviceIdentity RakDeviceIdentityProvider::read() const {
  uint8_t board_id[8]{};
  BoardGetUniqueId(board_id);
  DeviceIdentity identity{};
  // BoardGetUniqueId always supplies exactly eight bytes on the pinned RAK
  // driver. Keep the checked portable decoder as the single byte-order rule.
  (void)deviceIdentityFromLegacyBytes(board_id, sizeof(board_id), &identity);
  return identity;
}

}  // namespace orun_tlp
