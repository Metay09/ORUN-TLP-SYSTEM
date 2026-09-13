#pragma once

#include <stdint.h>

namespace orun_tlp {

class WatchdogManager {
 public:
  struct BootInfo {
    uint32_t reset_reason = 0;
    bool watchdog_reset = false;
  };

  static constexpr uint32_t kTimeoutSeconds = 30;

  static void begin();
  static void feed();
  static const BootInfo& bootInfo();

 private:
  static BootInfo boot_info_;
};

}  // namespace orun_tlp
