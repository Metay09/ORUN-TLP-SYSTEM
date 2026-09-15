#pragma once

namespace orun_tlp {

// Compatibility projection of today's legacy role behavior. Relay forwarding
// is represented as an independent enablement value so future configuration can
// control it without changing application/profile responsibilities.
struct LegacyRoleBehavior {
  constexpr LegacyRoleBehavior(bool relay_forwarding_enabled_value = false,
                               bool publish_gnss_position_value = false,
                               bool receive_application_position_value = false)
      : relay_forwarding_enabled(relay_forwarding_enabled_value),
        publish_gnss_position(publish_gnss_position_value),
        receive_application_position(receive_application_position_value) {}

  bool relay_forwarding_enabled;
  bool publish_gnss_position;
  bool receive_application_position;
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
