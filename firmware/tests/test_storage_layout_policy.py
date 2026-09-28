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

print("M6D3A storage ceiling parser checks: PASS")
