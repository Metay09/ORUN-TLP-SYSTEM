#pragma once
#include <stdint.h>

namespace orun_tlp {

// Supplies one fresh nonzero ConfigStore state-incarnation identifier.
//
// This is deliberately narrower than a generic RNG/HAL abstraction: ConfigStore
// needs exactly one 64-bit nonce-like namespace identifier when establishing a
// fresh v2 baseline/re-baseline. The token is not secret, but the architecture
// requires platform CSPRNG entropy so stale cached tokens cannot predictably
// collide across reset/re-baseline lifetimes.
class ConfigIncarnationSource {
 public:
  virtual ~ConfigIncarnationSource() = default;
  virtual bool generate(uint64_t& incarnation) = 0;
};

// RAK4630/nRF52840 production source.
//
// Current v2 clean-cutover scope calls this only during ConfigStore::begin(),
// before Bluefruit enables SoftDevice. It fails closed if SoftDevice is already
// enabled. A later runtime re-baseline path that needs fresh entropy while
// SoftDevice is active requires a separately reviewed source/ownership path.
class NrfConfigIncarnationSource : public ConfigIncarnationSource {
 public:
  bool generate(uint64_t& incarnation) override;
};

}  // namespace orun_tlp
