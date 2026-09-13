#pragma once

#include <stddef.h>
#include <stdint.h>

namespace orun_tlp {

enum class NodeRole : uint8_t { kTracker, kRelay, kBase };

const char* roleName(NodeRole role);

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
