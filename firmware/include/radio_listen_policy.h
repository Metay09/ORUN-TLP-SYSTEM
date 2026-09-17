#pragma once

#include <stdint.h>

namespace orun_tlp {

enum class RadioListenPolicy : uint8_t {
  kContinuous,
  kWindowed,
};

// Pure availability policy. Callers provide already-resolved service/runtime
// commitments; this layer deliberately knows nothing about NodeRole names.
RadioListenPolicy resolveRadioListenPolicy(bool relay_forwarding_running,
                                           bool receives_application_traffic,
                                           bool role_transition_pending);

const char* radioListenPolicyName(RadioListenPolicy policy);

}  // namespace orun_tlp
