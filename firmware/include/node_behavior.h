#pragma once

#include <stdint.h>

namespace orun_tlp {

// Network forwarding is independent of application role/profile and hardware
// capability. B3 only needs the two responsibilities already proven by M5.
enum class ForwardingResponsibility : uint8_t {
  kEndNode,
  kRelay,
};

// Compatibility projection of the current TRACKER/RELAY/BASE shorthand. This
// is not a new persisted configuration schema; it only makes today's coupled
// behavior explicit so later configuration work does not infer responsibilities
// directly from the legacy enum.
struct LegacyRoleBehavior {
  ForwardingResponsibility forwarding = ForwardingResponsibility::kEndNode;
  bool publish_gnss_position = false;
  bool receive_application_position = false;
};

constexpr bool operator==(LegacyRoleBehavior lhs, LegacyRoleBehavior rhs) {
  return lhs.forwarding == rhs.forwarding &&
         lhs.publish_gnss_position == rhs.publish_gnss_position &&
         lhs.receive_application_position == rhs.receive_application_position;
}

constexpr bool operator!=(LegacyRoleBehavior lhs, LegacyRoleBehavior rhs) {
  return !(lhs == rhs);
}

}  // namespace orun_tlp
