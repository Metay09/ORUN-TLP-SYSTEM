#pragma once

#include <stdint.h>

#define NRF_SUCCESS 0UL

// While the SoftDevice is enabled the application must not touch POWER
// directly. The fake keeps a separate SoftDevice-owned register so a test can
// tell which path production code took.
inline uint8_t fake_softdevice_enabled = 0;
inline uint32_t sd_softdevice_is_enabled(uint8_t* enabled) {
  *enabled = fake_softdevice_enabled;
  return NRF_SUCCESS;
}
