#!/usr/bin/env python3
"""SF5D non-mutating product-region flash geometry check.

Reads only the current source header. SF5D5 reserved the region in
storage_config.h; this check proves only the static source geometry, not that
an installed bootloader/DFU path preserves the pages (SF5D3/SF5D4 evidence).
"""
from pathlib import Path
import re

import storage_layout_policy as policy


FIRMWARE = Path(__file__).resolve().parents[1]
HEADER = FIRMWARE / "include" / "storage_config.h"

PRODUCT_START = 0x0C5000
PRODUCT_END = 0x0E5000
FLASH_LIMIT = 0x100000
BOOTLOADER_START = 0x0F4000


def verify(source: str) -> dict[str, tuple[int, int]]:
    """Fail closed on drift from the audited reserved layout."""
    # The preexisting post-link policy checks the application-to-geofence alias
    # and the geofence-to-security boundary, rejecting stale commented aliases.
    policy.application_policy_ceiling(source)
    active = policy._strip_comments(source)
    policy._reject_conditionals(active)

    page_size = policy._literal(active, "kPageSize")
    product_start = policy._literal(active, "kProductRegionStart")
    product_pages = policy._literal(active, "kProductRegionPages")
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
        ("kProductRegionEnd", "kProductRegionStart + kProductRegionPages * kPageSize"),
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
        ("kApplicationPolicyEndAddress", "kProductRegionStart"),
        ("kProductRegionEnd", "kGeofenceRegionStart"),
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
        or product_pages != 32
        or (geofence_pages, security_pages, config_pages, bond_pages)
        != (2, 2, 2, 2)
    ):
        raise RuntimeError("SF5D audited owner/page-size constants changed")

    ranges = {
        "product": (product_start, product_start + product_pages * page_size),
        "geofence": (geofence_start, geofence_start + geofence_pages * page_size),
        "security": (security_start, security_start + security_pages * page_size),
        "config": (security_start + security_pages * page_size,
                   security_start + (security_pages + config_pages) * page_size),
        "bond": (security_start + (security_pages + config_pages) * page_size,
                 security_start + (security_pages + config_pages + bond_pages) * page_size),
        "history": (history_start, history_start + history_pages * page_size),
    }

    if ranges["product"] != (PRODUCT_START, PRODUCT_END):
        raise RuntimeError("SF5D product region geometry changed")
    if ranges["geofence"][0] != PRODUCT_END:
        raise RuntimeError("product region no longer immediately precedes geofence")
    if ranges["history"][1] != BOOTLOADER_START or BOOTLOADER_START >= FLASH_LIMIT:
        raise RuntimeError("history/bootloader boundary changed")

    previous = None
    for name, (start, end) in ranges.items():
        if start % page_size or end % page_size or start >= end:
            raise RuntimeError(f"{name}: invalid page alignment or size")
        if previous is not None and start != previous:
            raise RuntimeError(f"{name}: gap/overlap in static flash geometry")
        previous = end

    # All checks above are only source-geometry checks; the application
    # ceiling (policy.application_policy_ceiling) is the product start.
    return ranges


def main() -> None:
    ranges = verify(HEADER.read_text(encoding="utf-8"))
    start, end = ranges["product"]
    print(
        f"SF5D product region geometry: PASS [{start:#08x},{end:#08x}) "
        f"{(end - start) // 1024} KiB / 32 pages, reserved"
    )
    print(
        "SF5D DFU preservation: serial single-bank PASS (SF5D4); "
        "interrupted/BLE DFU: OPEN; production writer: NONE"
    )


if __name__ == "__main__":
    main()
