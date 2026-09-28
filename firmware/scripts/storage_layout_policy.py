"""Pure parser for ORUN\'s application-policy flash ceiling.

Kept outside the PlatformIO/SCons script so host tests can prove that a stale
M7P2 0x0E7000 SecurityStore boundary cannot silently remain the real build
guard after M6D3A reserves GeofenceStore at 0x0E5000.
"""
import re


def _literal(text, symbol):
    match = re.search(
        rf"constexpr\\s+uint32_t\\s+{re.escape(symbol)}\\s*=\\s*"
        r"(0[xX][0-9A-Fa-f]+|[0-9]+)\\s*;",
        text,
    )
    if not match:
        raise RuntimeError(f"storage layout symbol {symbol} not found as a literal")
    return int(match.group(1), 0)


def _require(text, pattern, description):
    if not re.search(pattern, text, re.MULTILINE):
        raise RuntimeError(f"storage layout contract missing: {description}")


def application_policy_ceiling(storage_config_text):
    page_size = _literal(storage_config_text, "kPageSize")
    geofence_start = _literal(storage_config_text, "kGeofenceRegionStart")
    geofence_pages = _literal(storage_config_text, "kGeofenceRegionPages")
    security_start = _literal(storage_config_text, "kFutureSecurityRegionStart")

    _require(
        storage_config_text,
        r"constexpr\\s+uint32_t\\s+kGeofenceRegionEnd\\s*=\\s*"
        r"kGeofenceRegionStart\\s*\\+\\s*kGeofenceRegionPages\\s*\\*\\s*kPageSize\\s*;",
        "derived kGeofenceRegionEnd",
    )
    _require(
        storage_config_text,
        r"constexpr\\s+uint32_t\\s+kApplicationPolicyEndAddress\\s*=\\s*"
        r"kGeofenceRegionStart\\s*;",
        "application ceiling aliases kGeofenceRegionStart",
    )
    _require(
        storage_config_text,
        r"static_assert\\s*\\(\\s*kApplicationPolicyEndAddress\\s*==\\s*"
        r"kGeofenceRegionStart\\s*,",
        "application/geofence ceiling static_assert",
    )
    _require(
        storage_config_text,
        r"static_assert\\s*\\(\\s*kGeofenceRegionEnd\\s*==\\s*"
        r"kFutureSecurityRegionStart\\s*,",
        "geofence/security contiguity static_assert",
    )

    if page_size != 4096:
        raise RuntimeError("M6D3A requires the audited 4096-byte flash page")
    if geofence_start != 0x0E5000:
        raise RuntimeError(
            f"M6D3A geofence start changed: 0x{geofence_start:06X}; re-audit layout"
        )
    if geofence_pages != 2:
        raise RuntimeError(
            f"M6D3A geofence page count changed: {geofence_pages}; re-audit layout"
        )
    if geofence_start + geofence_pages * page_size != security_start:
        raise RuntimeError("geofence region no longer directly abuts SecurityStore")

    return geofence_start
