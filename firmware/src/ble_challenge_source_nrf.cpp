// 4c-1b: challenge bytes for authenticated Bluetooth writes. While BLE runs
// the SoftDevice owns the hardware RNG; its application pool is the only
// sanctioned runtime source (the boot-time incarnation sources read NRF_RNG
// directly, before the SoftDevice is enabled).
#include "ble_write_auth.h"

#include <nrf_sdm.h>
#include <nrf_soc.h>

namespace orun_tlp {

bool NrfBleChallengeSource::generate(
    uint8_t (&out)[ble_write_auth::kChallengeSize]) {
  uint8_t enabled = 0;
  if (sd_softdevice_is_enabled(&enabled) != NRF_SUCCESS || enabled == 0)
    return false;
  uint8_t available = 0;
  if (sd_rand_application_bytes_available_get(&available) != NRF_SUCCESS ||
      available < sizeof(out))
    return false;
  if (sd_rand_application_vector_get(out, sizeof(out)) != NRF_SUCCESS)
    return false;
  // An all-zero answer is never a usable challenge.
  uint8_t any = 0;
  for (uint8_t b : out) any = static_cast<uint8_t>(any | b);
  return any != 0;
}

}  // namespace orun_tlp
