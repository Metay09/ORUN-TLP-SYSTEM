#include "config_incarnation_source.h"

#include <Arduino.h>
#include <nrf.h>
#include <nrf_sdm.h>
#include <nrf_soc.h>

namespace orun_tlp {
namespace {

constexpr uint32_t kByteTimeoutMs = 100;
constexpr unsigned kIncarnationBytes = sizeof(uint64_t);

// nRF52840 PS: RNG CONFIG.DERCEN bit 0 enables hardware bias correction.
// Keep this literal local rather than depending on a vendor enum spelling
// that differs across Nordic header generations.
constexpr uint32_t kBiasCorrectionEnabled = 1U;

bool softDeviceDisabled() {
  uint8_t enabled = 1;
  return sd_softdevice_is_enabled(&enabled) == NRF_SUCCESS && enabled == 0;
}

bool nextByte(uint8_t& value) {
  const uint32_t started = millis();
  while (NRF_RNG->EVENTS_VALRDY == 0) {
    if (static_cast<uint32_t>(millis() - started) >= kByteTimeoutMs)
      return false;
  }
  NRF_RNG->EVENTS_VALRDY = 0;
  value = static_cast<uint8_t>(NRF_RNG->VALUE);
  return true;
}

}  // namespace

bool NrfConfigIncarnationSource::generate(uint64_t& incarnation) {
  // The current production contract establishes fresh ConfigStore identity
  // before Bluefruit/SoftDevice startup. Never touch the raw RNG peripheral
  // after SoftDevice owns SoC resources.
  if (!softDeviceDisabled()) return false;

  const uint32_t saved_config = NRF_RNG->CONFIG;
  NRF_RNG->CONFIG = kBiasCorrectionEnabled;
  NRF_RNG->EVENTS_VALRDY = 0;
  NRF_RNG->TASKS_START = 1;

  uint64_t candidate = 0;
  bool ok = true;
  for (unsigned i = 0; i < kIncarnationBytes; ++i) {
    uint8_t byte = 0;
    if (!nextByte(byte)) {
      ok = false;
      break;
    }
    candidate = (candidate << 8) | static_cast<uint64_t>(byte);
  }

  NRF_RNG->TASKS_STOP = 1;
  NRF_RNG->CONFIG = saved_config;

  if (!ok || candidate == 0) return false;
  incarnation = candidate;
  return true;
}

}  // namespace orun_tlp
