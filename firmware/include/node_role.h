#pragma once

#include <stddef.h>
#include <stdint.h>

#include "node_behavior.h"

namespace orun_tlp {

enum class NodeRole : uint8_t { kTracker, kRelay, kBase };

const char* roleName(NodeRole role);

// Freeze today's legacy role semantics without turning NodeRole into the future
// configuration model. In particular, relay forwarding is an independent
// behavior value rather than a permanent node classification.
inline LegacyRoleBehavior legacyRoleBehavior(NodeRole role) {
  switch (role) {
    case NodeRole::kTracker:
      return LegacyRoleBehavior(false, true, false);
    case NodeRole::kRelay:
      return LegacyRoleBehavior(true, false, false);
    case NodeRole::kBase:
      return LegacyRoleBehavior(false, false, true);
  }
  return LegacyRoleBehavior(false, false, true);
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
  // Return to GNSS-based inference. The role is kept until the next
  // updateAutomatic() call decides it again.
  void restoreAutomatic() { automatic_ = true; }

 private:
  NodeRole role_ = NodeRole::kBase;
  bool automatic_ = true;
};

}  // namespace orun_tlp
