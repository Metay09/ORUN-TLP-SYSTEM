#pragma once

#include <stdint.h>

#include "nrf_sdm.h"

inline uint32_t fake_softdevice_gpregret[2]{};
inline unsigned fake_softdevice_gpregret_calls = 0;

inline uint32_t sd_power_gpregret_get(uint32_t id, uint32_t* value) {
  ++fake_softdevice_gpregret_calls;
  *value = fake_softdevice_gpregret[id];
  return NRF_SUCCESS;
}
inline uint32_t sd_power_gpregret_clr(uint32_t id, uint32_t mask) {
  ++fake_softdevice_gpregret_calls;
  fake_softdevice_gpregret[id] &= ~mask;
  return NRF_SUCCESS;
}
inline uint32_t sd_power_gpregret_set(uint32_t id, uint32_t mask) {
  ++fake_softdevice_gpregret_calls;
  fake_softdevice_gpregret[id] |= mask;
  return NRF_SUCCESS;
}
