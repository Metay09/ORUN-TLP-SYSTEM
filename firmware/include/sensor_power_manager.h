#pragma once

#include <stdint.h>

namespace orun_tlp {

enum class SensorPowerOwner : uint8_t {
  kGnss = 0,
  kAuxiliary = 1,
};

class SensorPowerManager {
 public:
  static void begin();
  static void acquire(SensorPowerOwner owner);
  static void release(SensorPowerOwner owner);
  static bool powered();
  static uint32_t ownerMask();

 private:
  static uint32_t owners_;
};

}  // namespace orun_tlp
