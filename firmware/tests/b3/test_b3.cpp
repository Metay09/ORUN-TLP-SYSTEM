#include <assert.h>
#include <stdio.h>

#include "node_role.h"

using namespace orun_tlp;

int main() {
  const auto tracker = legacyRoleBehavior(NodeRole::kTracker);
  assert(!tracker.relay_forwarding_enabled);
  assert(tracker.publish_gnss_position);
  assert(!tracker.receive_application_position);

  const auto relay = legacyRoleBehavior(NodeRole::kRelay);
  assert(relay.relay_forwarding_enabled);
  assert(!relay.publish_gnss_position);
  assert(!relay.receive_application_position);

  const auto base = legacyRoleBehavior(NodeRole::kBase);
  assert(!base.relay_forwarding_enabled);
  assert(!base.publish_gnss_position);
  assert(base.receive_application_position);

  RoleController roles;
  assert(roles.role() == NodeRole::kBase && roles.automatic());
  assert(!roles.updateAutomatic(false, true));
  assert(roles.updateAutomatic(true, true));
  assert(roles.role() == NodeRole::kTracker && roles.automatic());
  assert(roles.applyOverride(NodeRole::kRelay));
  assert(!roles.automatic() && roles.role() == NodeRole::kRelay);
  assert(!roles.updateAutomatic(true, false));
  assert(roles.role() == NodeRole::kRelay);

  puts("B3 legacy role compatibility mapping: PASS");
}
