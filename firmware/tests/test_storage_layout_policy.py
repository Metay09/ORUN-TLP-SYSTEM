#!/usr/bin/env python3
"""M6D3A application-ceiling parser regression."""
from pathlib import Path
import importlib.util

ROOT = Path(__file__).resolve().parents[1]
HELPER = ROOT / "scripts" / "storage_layout_policy.py"
HEADER = ROOT / "include" / "storage_config.h"

spec = importlib.util.spec_from_file_location("storage_layout_policy", HELPER)
policy = importlib.util.module_from_spec(spec)
spec.loader.exec_module(policy)

text = HEADER.read_text()
assert policy.application_policy_ceiling(text) == 0x0E5000
assert "kFutureSecurityRegionStart = 0x0E7000" in text

bad_alias = text.replace(
    "kApplicationPolicyEndAddress = kGeofenceRegionStart",
    "kApplicationPolicyEndAddress = kFutureSecurityRegionStart",
)
try:
    policy.application_policy_ceiling(bad_alias)
    raise AssertionError("stale application ceiling alias was accepted")
except RuntimeError:
    pass

bad_chain = text.replace(
    "kGeofenceRegionEnd == kFutureSecurityRegionStart",
    "kGeofenceRegionEnd != kFutureSecurityRegionStart",
)
try:
    policy.application_policy_ceiling(bad_chain)
    raise AssertionError("broken geofence/security chain was accepted")
except RuntimeError:
    pass

# Stale audited declarations in comments must never override a changed active
# layout. This models the audit probe that previously fooled the raw regex.
comment_stale = (
    "// constexpr uint32_t kGeofenceRegionPages = 2;\n"
    "// constexpr uint32_t kGeofenceRegionStart = 0x0E5000;\n"
    + text.replace(
        "constexpr uint32_t kGeofenceRegionPages = 2;",
        "constexpr uint32_t kGeofenceRegionPages = 4;",
    ).replace(
        "constexpr uint32_t kGeofenceRegionStart = 0x0E5000;",
        "constexpr uint32_t kGeofenceRegionStart = 0x0E3000;",
    )
)
try:
    policy.application_policy_ceiling(comment_stale)
    raise AssertionError("commented stale layout declarations were accepted")
except RuntimeError:
    pass

# Ambiguous active declarations fail closed even though C++ compilation would
# also reject them; the policy parser must never guess which declaration wins.
duplicate = text + "\nconstexpr uint32_t kGeofenceRegionStart = 0x0E5000;\n"
try:
    policy.application_policy_ceiling(duplicate)
    raise AssertionError("duplicate active layout declaration was accepted")
except RuntimeError:
    pass

# Preprocessor-conditional layout/assertion definitions are intentionally
# unsupported so #if 0 blocks cannot satisfy the parser while being inactive
# for the compiler.
conditional = (
    "#if 0\n"
    "static_assert(kGeofenceRegionEnd == kFutureSecurityRegionStart, "
    "\"disabled stale assertion\");\n"
    "#endif\n"
    + text
)
try:
    policy.application_policy_ceiling(conditional)
    raise AssertionError("conditional layout source was accepted")
except RuntimeError:
    pass

# Commented aliases/assertions do not count as active contract evidence.
commented_assert = text.replace(
    "static_assert(kGeofenceRegionEnd == kFutureSecurityRegionStart,",
    "// static_assert(kGeofenceRegionEnd == kFutureSecurityRegionStart,",
)
try:
    policy.application_policy_ceiling(commented_assert)
    raise AssertionError("commented contiguity assertion was accepted")
except RuntimeError:
    pass

print("M6D3A storage ceiling parser checks: PASS")
