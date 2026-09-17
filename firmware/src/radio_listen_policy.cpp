#include "radio_listen_policy.h"

namespace orun_tlp {

RadioListenPolicy resolveRadioListenPolicy(bool relay_forwarding_running,
                                           bool receives_application_traffic,
                                           bool role_transition_pending) {
  return relay_forwarding_running || receives_application_traffic ||
                 role_transition_pending
             ? RadioListenPolicy::kContinuous
             : RadioListenPolicy::kWindowed;
}

const char* radioListenPolicyName(RadioListenPolicy policy) {
  return policy == RadioListenPolicy::kContinuous ? "CONTINUOUS" : "WINDOWED";
}

}  // namespace orun_tlp
