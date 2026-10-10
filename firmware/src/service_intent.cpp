#include "service_intent.h"

#include "config_format.h"

namespace orun_tlp {
namespace cf = config_format;

ServiceIntentAdmission admitServiceIntent(uint8_t service_mode,
                                          uint8_t requested_services) {
  if (service_mode == cf::kServiceModeAuto)
    return requested_services == 0 ? ServiceIntentAdmission::kSupported
                                   : ServiceIntentAdmission::kInvalid;
  if (service_mode != cf::kServiceModeExplicit ||
      (requested_services & static_cast<uint8_t>(~cf::kKnownServicesMask)) != 0)
    return ServiceIntentAdmission::kInvalid;

  const bool receive = (requested_services & cf::kServiceApplicationReceive) != 0;
  const bool other = (requested_services & static_cast<uint8_t>(
                          cf::kServiceTracking | cf::kServiceRelayForwarding)) != 0;
  if (receive && other) return ServiceIntentAdmission::kUnsupportedCombination;
  return ServiceIntentAdmission::kSupported;
}

NodeRole legacyRoleForServices(uint8_t requested_services) {
  if ((requested_services & cf::kServiceApplicationReceive) != 0)
    return NodeRole::kBase;
  // Relay without tracking keeps the legacy relay carrier. Tracking (with or
  // without relay forwarding, and the explicit "nothing" intent) uses the
  // tracker carrier; relay forwarding is then switched on as its own service.
  if (requested_services == cf::kServiceRelayForwarding) return NodeRole::kRelay;
  return NodeRole::kTracker;
}

RequestedConfig requestedConfigForServices(uint8_t requested_services) {
  const bool tracking = (requested_services & cf::kServiceTracking) != 0;
  const bool relay = (requested_services & cf::kServiceRelayForwarding) != 0;
  return RequestedConfig(tracking, relay,
                         tracking ? RequestedLocationSource::kGnss
                                  : RequestedLocationSource::kNone);
}

bool parseServiceIntent(const char* text, size_t length,
                        uint8_t* service_mode, uint8_t* requested_services) {
  if (text == nullptr || service_mode == nullptr ||
      requested_services == nullptr || length == 0)
    return false;
  if (length == 4 && text[0] == 'A' && text[1] == 'U' && text[2] == 'T' &&
      text[3] == 'O') {
    *service_mode = cf::kServiceModeAuto;
    *requested_services = 0;
    return true;
  }
  if (length == 4 && text[0] == 'N' && text[1] == 'O' && text[2] == 'N' &&
      text[3] == 'E') {
    *service_mode = cf::kServiceModeExplicit;
    *requested_services = 0;
    return true;
  }
  uint8_t services = 0;
  for (size_t i = 0; i < length; ++i) {
    uint8_t bit = 0;
    switch (text[i]) {
      case 'T': bit = cf::kServiceTracking; break;
      case 'R': bit = cf::kServiceRelayForwarding; break;
      case 'A': bit = cf::kServiceApplicationReceive; break;
      default: return false;
    }
    if ((services & bit) != 0) return false;  // each letter at most once
    services = static_cast<uint8_t>(services | bit);
  }
  *service_mode = cf::kServiceModeExplicit;
  *requested_services = services;
  return true;
}

const char* formatServices(uint8_t requested_services, char* out,
                           size_t size) {
  if (out == nullptr || size < 5) return "";
  if ((requested_services & cf::kKnownServicesMask) == 0) {
    out[0] = 'N'; out[1] = 'O'; out[2] = 'N'; out[3] = 'E'; out[4] = '\0';
    return out;
  }
  size_t n = 0;
  if (requested_services & cf::kServiceTracking) out[n++] = 'T';
  if (requested_services & cf::kServiceRelayForwarding) out[n++] = 'R';
  if (requested_services & cf::kServiceApplicationReceive) out[n++] = 'A';
  out[n] = '\0';
  return out;
}

}  // namespace orun_tlp
