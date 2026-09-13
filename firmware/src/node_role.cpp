#include "node_role.h"

namespace orun_tlp {
namespace {

char upper(char value) {
  return value >= 'a' && value <= 'z' ? static_cast<char>(value - 'a' + 'A')
                                      : value;
}

bool equals(const char* input, size_t length, const char* expected) {
  size_t index = 0;
  while (expected[index] != '\0') {
    if (index >= length || upper(input[index]) != expected[index]) return false;
    ++index;
  }
  return index == length;
}

}  // namespace

const char* roleName(NodeRole role) {
  switch (role) {
    case NodeRole::kTracker: return "TRACKER";
    case NodeRole::kRelay: return "RELAY";
    case NodeRole::kBase: return "BASE";
  }
  return "BASE";
}

RoleCommand parseRoleCommand(const char* line, size_t length) {
  if (line == nullptr) return RoleCommand::kInvalid;
  while (length && (*line == ' ' || *line == '\t')) { ++line; --length; }
  while (length && (line[length - 1] == ' ' || line[length - 1] == '\t')) --length;
  if (equals(line, length, "ROLE?")) return RoleCommand::kQuery;
  if (equals(line, length, "ROLE TRACKER")) return RoleCommand::kTracker;
  if (equals(line, length, "ROLE RELAY")) return RoleCommand::kRelay;
  if (equals(line, length, "ROLE BASE")) return RoleCommand::kBase;
  return length == 0 ? RoleCommand::kNone : RoleCommand::kInvalid;
}

bool RoleController::updateAutomatic(bool detection_complete,
                                     bool gnss_detected) {
  if (!automatic_ || !detection_complete) return false;
  const NodeRole next = gnss_detected ? NodeRole::kTracker : NodeRole::kBase;
  const bool changed = next != role_;
  role_ = next;
  return changed;
}

bool RoleController::applyOverride(NodeRole role) {
  const bool changed = automatic_ || role != role_;
  automatic_ = false;
  role_ = role;
  return changed;
}

}  // namespace orun_tlp
