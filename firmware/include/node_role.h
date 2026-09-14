#pragma once

#include <stddef.h>
#include <stdint.h>

#include "node_behavior.h"

namespace orun_tlp {

enum class NodeRole : uint8_t { kTracker, kRelay, kBase };

const char* roleName(NodeRole role);

// Freeze today's legacy role semantics without turning the role enum into the
// future configuration model. New code can reason about forwarding and current
// POSITION services independently while legacy commands remain unchanged.
constexpr LegacyRoleBehavior legacyRoleBehavior(NodeRole role) {
  switch (role) {
    case NodeRole::kTracker:
      return {ForwardingResponsibility::kEndNode, true, false};
    case NodeRole::kRelay:
      return {ForwardingResponsibility::kRelay, false, false};
    case NodeRole::kBase:
      return {ForwardingResponsibility::kEndNode, false, true};
  }
  return {ForwardingResponsibility::kEndNode, false, true};
}

enum class RoleCommand : uint8_t {
  kNone,
  kQuery,
  kTracker,
  kRelay,
  kBase,
  kInvalid,
};

RoleCommand parseRoleCommand(const char* line, size_t length);

class RoleController {
 public:
  NodeRole role() const { return role_; }
  bool automatic() const { return automatic_; }
  bool updateAutomatic(bool detection_complete, bool gnss_detected);
  bool applyOverride(NodeRole role);

 private:
  NodeRole role_ = NodeRole::kBase;
  bool automatic_ = true;
};

}  // namespace orun_tlp
