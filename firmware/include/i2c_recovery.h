#pragma once

#include <stdint.h>

namespace orun_tlp {

enum class I2cRecoveryResult : uint8_t {
  kNoTimeout,
  kRecovered,
  kFailed,
};

class I2cRecovery {
 public:
  static I2cRecoveryResult serviceTimeout();

 private:
  static bool recoverBus();
};

}  // namespace orun_tlp
