#!/usr/bin/env python3
"""SF5D non-mutating candidate flash geometry check.

Reads only the current source header. This neither reserves memory nor proves
the installed bootloader/DFU preserves pages during firmware updates.
"""
from pathlib import Path
import re

import storage_layout_policy as policy


FIRMWARE = Path(__file__).resolve().parents[1]
HEADER = FIRMWARE / "include" / "storage_config.h"

CANDIDATE_START = 0x0C5000
CANDIDATE_END = 0x0E5000
FLASH_LIMIT = 0x100000
BOOTLOADER_START = 0x0F4000


def verify(source: str) -> dict[str, tuple[int, int]]:
    """Fail closed on drift from the currently audited, *unallocated* layout."""
    # The preexisting post-link policy checks the application-to-geofence alias
    # and the geofence-to-security boundary, rejecting stale commented aliases.
    policy.application_policy_ceiling(source)
    active = policy._strip_comments(source)
    policy._reject_conditionals(active)

    page_size = policy._literal(active, "kPageSize")
    history_start = policy._literal(active, "kBaseAddress")
    history_pages = policy._literal(active, "kPageCount")
    geofence_start = policy._literal(active, "kGeofenceRegionStart")
    geofence_pages = policy._literal(active, "kGeofenceRegionPages")
    security_start = policy._literal(active, "kFutureSecurityRegionStart")
    security_pages = policy._literal(active, "kFutureSecurityRegionPages")
    config_pages = policy._literal(active, "kFutureConfigRegionPages")
    bond_pages = policy._literal(active, "kFutureBondRegionPages")

    # Validate the literal C++ address derivations and compile-time adjacency
    # assertions as well as their computed values: no dormant/stale assertions.
    for symbol, parent in (
        ("kGeofenceRegionEnd", "kGeofenceRegionStart + kGeofenceRegionPages * kPageSize"),
        ("kFutureSecurityRegionEnd", "kFutureSecurityRegionStart + kFutureSecurityRegionPages * kPageSize"),
        ("kFutureConfigRegionStart", "kFutureSecurityRegionEnd"),
        ("kFutureConfigRegionEnd", "kFutureConfigRegionStart + kFutureConfigRegionPages * kPageSize"),
        ("kFutureBondRegionStart", "kFutureConfigRegionEnd"),
        ("kFutureBondRegionEnd", "kFutureBondRegionStart + kFutureBondRegionPages * kPageSize"),
    ):
        # The source expressions above are intentionally fixed, not evaluated.
        expression = r"\s*".join(re.escape(token) for token in parent.split())
        policy._require_unique(
            active,
            rf"constexpr\s+uint32_t\s+{re.escape(symbol)}\s*=\s*{expression}\s*;",
            f"derived {symbol}",
        )

    for left, right in (
        ("kApplicationPolicyEndAddress", "kGeofenceRegionStart"),
        ("kGeofenceRegionEnd", "kFutureSecurityRegionStart"),
        ("kFutureSecurityRegionEnd", "kFutureConfigRegionStart"),
        ("kFutureConfigRegionEnd", "kFutureBondRegionStart"),
        ("kFutureBondRegionEnd", "kBaseAddress"),
    ):
        policy._require_unique(
            active,
            rf"static_assert\s*\(\s*{re.escape(left)}\s*==\s*{re.escape(right)}\s*,",
            f"adjacency assertion {left} == {right}",
        )

    if (
        page_size != 4096
        or history_start != 0x0ED000
        or history_pages != 7
        or (geofence_pages, security_pages, config_pages, bond_pages)
        != (2, 2, 2, 2)
    ):
        raise RuntimeError("SF5D audited owner/page-size constants changed")

    ranges = {
        "candidate_unassigned": (CANDIDATE_START, CANDIDATE_END),
        "geofence": (geofence_start, geofence_start + geofence_pages * page_size),
        "security": (security_start, security_start + security_pages * page_size),
        "config": (security_start + security_pages * page_size,
                   security_start + (security_pages + config_pages) * page_size),
        "bond": (security_start + (security_pages + config_pages) * page_size,
                 security_start + (security_pages + config_pages + bond_pages) * page_size),
        "history": (history_start, history_start + history_pages * page_size),
    }

    if ranges["candidate_unassigned"] != (0x0C5000, 0x0E5000):
        raise RuntimeError("SF5D candidate geometry changed")
    if ranges["geofence"][0] != CANDIDATE_END:
        raise RuntimeError("candidate no longer immediately precedes geofence")
    if ranges["history"][1] != BOOTLOADER_START or BOOTLOADER_START >= FLASH_LIMIT:
        raise RuntimeError("history/bootloader boundary changed")

    previous = None
    for name, (start, end) in ranges.items():
        if start % page_size or end % page_size or start >= end:
            raise RuntimeError(f"{name}: invalid page alignment or size")
        if previous is not None and start != previous:
            raise RuntimeError(f"{name}: gap/overlap in static flash geometry")
        previous = end

    # All checks above are only source-geometry checks. In particular the
    # current app build ceiling is STILL 0xE5000, not the candidate start!
    # Never report the candidate as allocated or DFU-safe.
    return ranges


def main() -> None:
    ranges = verify(HEADER.read_text(encoding="utf-8"))
    start, end = ranges["candidate_unassigned"]
    print(
        f"SF5D candidate geometry: PASS [{start:#08x},{end:#08x}) "
        f"{(end - start) // 1024} KiB / 32 pages"
    )
    print(
        "SF5D DFU preservation: OPEN; hybrid owner: OPEN; "
        "candidate NOT reserved; device writes: NONE"
    )


if __name__ == "__main__":
    main()
