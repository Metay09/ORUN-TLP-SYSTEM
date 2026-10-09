#!/usr/bin/env python3
"""SF5D preflight regression: only static source geometry is being proved."""
from pathlib import Path
import sys

FIRMWARE = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(FIRMWARE / "scripts"))
import sf5d_layout_preflight as preflight  # noqa: E402

header = (FIRMWARE / "include" / "storage_config.h").read_text(encoding="utf-8")
regions = preflight.verify(header)
assert regions == {
    "candidate_unassigned": (0x0C5000, 0x0E5000),
    "geofence": (0x0E5000, 0x0E7000),
    "security": (0x0E7000, 0x0E9000),
    "config": (0x0E9000, 0x0EB000),
    "bond": (0x0EB000, 0x0ED000),
    "history": (0x0ED000, 0x0F4000),
}


def reject(name: str, old: str, new: str) -> None:
    if header.count(old) != 1:
        raise AssertionError(f"{name}: mutation anchor absent or ambiguous")
    mutated = header.replace(old, new, 1)
    try:
        preflight.verify(mutated)
    except RuntimeError:
        pass
    else:
        raise AssertionError(f"{name}: unsafe layout source accepted")


reject(
    "expanded Config overlaps History",
    "constexpr uint32_t kFutureConfigRegionPages = 2;",
    "constexpr uint32_t kFutureConfigRegionPages = 3;",
)
reject(
    "deceptive config-start alias",
    "kFutureConfigRegionStart = kFutureSecurityRegionEnd;",
    "kFutureConfigRegionStart = kGeofenceRegionEnd;",
)
reject(
    "missing bond/history contiguity proof",
    "kFutureBondRegionEnd == kBaseAddress,",
    "kFutureBondRegionEnd != kBaseAddress,",
)
reject(
    "geofence overlaps candidate",
    "constexpr uint32_t kGeofenceRegionStart = 0x0E5000;",
    "constexpr uint32_t kGeofenceRegionStart = 0x0E3000;",
)
reject(
    "policy ceiling moved above protected data",
    "kApplicationPolicyEndAddress = kGeofenceRegionStart;",
    "kApplicationPolicyEndAddress = kFutureSecurityRegionStart;",
)
reject(
    "one extra History page would consume bootloader",
    "constexpr uint32_t kPageCount = 7;",
    "constexpr uint32_t kPageCount = 8;",
)

# A stale commented declaration cannot be used as an alternative to the
# active immutable candidate-protection chain.
reject(
    "silently changed security size",
    "constexpr uint32_t kFutureSecurityRegionPages = 2;",
    "constexpr uint32_t kFutureSecurityRegionPages = 3;",
)

print("SF5D source-layout preflight mutations: PASS (no hardware / DFU claims)")
