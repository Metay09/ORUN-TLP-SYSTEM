#pragma once

#include "device_identity.h"

namespace orun_tlp {

// RAK4630/4631 board adapter for the stable legacy device identity. Hardware
// register layout remains hidden behind BoardGetUniqueId in the pinned driver.
class RakDeviceIdentityProvider {
 public:
  DeviceIdentity read() const;
};

}  // namespace orun_tlp
