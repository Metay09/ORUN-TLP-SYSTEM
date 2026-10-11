#pragma once

#include <stddef.h>
#include <stdint.h>

#include "node_role.h"
#include "runtime_config.h"

namespace orun_tlp {

// Maps the persisted requested-service intent (ConfigStore schema v4,
// docs/architecture/ORUN_CONFIG_STORE_V4_SERVICE_INTENT.md) onto the runtime
// that exists today.
//
// AUTO keeps the legacy behaviour: the role is inferred from GNSS presence.
// EXPLICIT replaces that inference: the requested services decide what runs,
// and a missing capability blocks a service instead of changing the device's
// job. The legacy NodeRole is kept only as the network/receive carrier the
// radio layer still expects; it is derived from the intent, never the other
// way round.
//
// Which combinations are admitted is a property of this runtime, not of the
// stored format: today the receive path and relay forwarding cannot share a
// device (relay forwarding takes every received POSITION), and tracking on a
// receiving device has never been exercised, so those are refused.

enum class ServiceIntentAdmission : uint8_t {
  kSupported,
  kInvalid,                 // unknown mode/bit, or AUTO with services
  kUnsupportedCombination,  // valid intent this runtime cannot run
};

ServiceIntentAdmission admitServiceIntent(uint8_t service_mode,
                                          uint8_t requested_services);

// Only meaningful for an admitted EXPLICIT intent.
NodeRole legacyRoleForServices(uint8_t requested_services);
RequestedConfig requestedConfigForServices(uint8_t requested_services);

// USB spelling. Services are letters, each at most once, any order:
//   T = tracking (Konum Takibi), R = relay forwarding (Aktarma),
//   A = application receive (alıcı).
// "AUTO" = no explicit selection; "NONE" = explicit, nothing requested.
// parseServiceIntent() accepts exactly these forms and changes nothing on
// failure. formatServices() writes "NONE" or the letters in T,R,A order.
bool parseServiceIntent(const char* text, size_t length,
                        uint8_t* service_mode, uint8_t* requested_services);
const char* formatServices(uint8_t requested_services, char* out, size_t size);

}  // namespace orun_tlp
