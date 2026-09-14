#pragma once

namespace orun_tlp {

// Compatibility projection of today's legacy role behavior. Relay forwarding
// is represented as an independent enablement value so future configuration can
// control it without changing application/profile responsibilities.
struct LegacyRoleBehavior {
  bool relay_forwarding_enabled = false;
  bool publish_gnss_position = false;
  bool receive_application_position = false;
};

constexpr bool operator==(LegacyRoleBehavior lhs, LegacyRoleBehavior rhs) {
  return lhs.relay_forwarding_enabled == rhs.relay_forwarding_enabled &&
         lhs.publish_gnss_position == rhs.publish_gnss_position &&
         lhs.receive_application_position == rhs.receive_application_position;
}

constexpr bool operator!=(LegacyRoleBehavior lhs, LegacyRoleBehavior rhs) {
  return !(lhs == rhs);
}

}  // namespace orun_tlp
