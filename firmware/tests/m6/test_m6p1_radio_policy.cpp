#include <assert.h>
#include <stdio.h>

#include "node_role.h"
#include "radio_config.h"
#include "radio_listen_policy.h"

using namespace orun_tlp;

int main() {
  // Owner-selected M6P2 development default. This exact-value check prevents
  // an accidental timing regression while the downlink contract is designed.
  assert(radio_config::kWindowedRxAfterTxMs == 10000);

  const auto tracker = legacyRoleBehavior(NodeRole::kTracker);
  const auto relay = legacyRoleBehavior(NodeRole::kRelay);
  const auto base = legacyRoleBehavior(NodeRole::kBase);

  assert(resolveRadioListenPolicy(
             tracker.relay_forwarding_enabled,
             tracker.receive_application_position, false) ==
         RadioListenPolicy::kWindowed);
  assert(resolveRadioListenPolicy(
             relay.relay_forwarding_enabled,
             relay.receive_application_position, false) ==
         RadioListenPolicy::kContinuous);
  assert(resolveRadioListenPolicy(
             base.relay_forwarding_enabled,
             base.receive_application_position, false) ==
         RadioListenPolicy::kContinuous);

  assert(resolveRadioListenPolicy(true, false, false) ==
         RadioListenPolicy::kContinuous);
  assert(resolveRadioListenPolicy(true, true, false) ==
         RadioListenPolicy::kContinuous);
  assert(resolveRadioListenPolicy(false, false, true) ==
         RadioListenPolicy::kContinuous);

  puts("M6P1 portable radio listen policy checks: PASS");
}
