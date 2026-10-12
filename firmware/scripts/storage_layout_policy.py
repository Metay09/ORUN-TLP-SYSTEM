"""Pure parser for ORUN's application-policy flash ceiling.

Kept outside the PlatformIO/SCons script so host tests can prove that stale,
commented or conditionally-disabled layout declarations cannot silently weaken
the application ceiling. M6D3A set it at GeofenceStore's 0x0E5000; SF5D5 moves
it down to the 128 KiB product region's 0x0C5000.
"""
import re


def _strip_comments(text):
    """Remove C/C++ comments while preserving string/char literal contents."""
    out = []
    i = 0
    state = "code"
    while i < len(text):
        ch = text[i]
        nxt = text[i + 1] if i + 1 < len(text) else ""

        if state == "code":
            if ch == "/" and nxt == "/":
                state = "line_comment"
                i += 2
                continue
            if ch == "/" and nxt == "*":
                state = "block_comment"
                i += 2
                continue
            if ch == '"':
                state = "string"
            elif ch == "'":
                state = "char"
            out.append(ch)
            i += 1
            continue

        if state == "line_comment":
            if ch == "\n":
                out.append("\n")
                state = "code"
            i += 1
            continue

        if state == "block_comment":
            if ch == "*" and nxt == "/":
                state = "code"
                i += 2
                continue
            if ch == "\n":
                out.append("\n")
            i += 1
            continue

        # Preserve quoted literals exactly so assertion messages cannot alter
        # token adjacency in the surrounding source.
        out.append(ch)
        if ch == "\\" and i + 1 < len(text):
            out.append(text[i + 1])
            i += 2
            continue
        if state == "string" and ch == '"':
            state = "code"
        elif state == "char" and ch == "'":
            state = "code"
        i += 1

    if state == "block_comment":
        raise RuntimeError("unterminated block comment in storage layout header")
    return "".join(out)


def _reject_conditionals(text):
    if re.search(
        r"^\s*#\s*(if|ifdef|ifndef|elif|else|endif)\b",
        text,
        re.MULTILINE,
    ):
        raise RuntimeError(
            "conditional storage layout declarations are unsupported; "
            "audit the layout explicitly"
        )


def _unique_match(text, pattern, description):
    matches = list(re.finditer(pattern, text, re.MULTILINE))
    if len(matches) != 1:
        raise RuntimeError(
            f"storage layout contract must contain exactly one {description}; "
            f"found {len(matches)}"
        )
    return matches[0]


def _literal(text, symbol):
    match = _unique_match(
        text,
        rf"constexpr\s+uint32_t\s+{re.escape(symbol)}\s*=\s*"
        r"(0[xX][0-9A-Fa-f]+|[0-9]+)\s*;",
        f"literal declaration for {symbol}",
    )
    return int(match.group(1), 0)


def _require_unique(text, pattern, description):
    _unique_match(text, pattern, description)


def application_policy_ceiling(storage_config_text):
    text = _strip_comments(storage_config_text)
    _reject_conditionals(text)

    page_size = _literal(text, "kPageSize")
    product_start = _literal(text, "kProductRegionStart")
    product_pages = _literal(text, "kProductRegionPages")
    geofence_start = _literal(text, "kGeofenceRegionStart")
    geofence_pages = _literal(text, "kGeofenceRegionPages")
    security_start = _literal(text, "kFutureSecurityRegionStart")

    _require_unique(
        text,
        r"constexpr\s+uint32_t\s+kProductRegionEnd\s*=\s*"
        r"kProductRegionStart\s*\+\s*kProductRegionPages\s*\*\s*kPageSize\s*;",
        "derived kProductRegionEnd",
    )
    _require_unique(
        text,
        r"constexpr\s+uint32_t\s+kGeofenceRegionEnd\s*=\s*"
        r"kGeofenceRegionStart\s*\+\s*kGeofenceRegionPages\s*\*\s*kPageSize\s*;",
        "derived kGeofenceRegionEnd",
    )
    _require_unique(
        text,
        r"constexpr\s+uint32_t\s+kApplicationPolicyEndAddress\s*=\s*"
        r"kProductRegionStart\s*;",
        "application ceiling alias",
    )
    _require_unique(
        text,
        r"static_assert\s*\(\s*kApplicationPolicyEndAddress\s*==\s*"
        r"kProductRegionStart\s*,",
        "application/product ceiling static_assert",
    )
    _require_unique(
        text,
        r"static_assert\s*\(\s*kProductRegionEnd\s*==\s*"
        r"kGeofenceRegionStart\s*,",
        "product/geofence contiguity static_assert",
    )
    _require_unique(
        text,
        r"static_assert\s*\(\s*kGeofenceRegionEnd\s*==\s*"
        r"kFutureSecurityRegionStart\s*,",
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
    if product_start != 0x0C5000 or product_pages != 32:
        raise RuntimeError(
            f"SF5D5 product region changed: 0x{product_start:06X}, "
            f"{product_pages} pages; re-audit layout"
        )
    if product_start + product_pages * page_size != geofence_start:
        raise RuntimeError("product region no longer directly abuts GeofenceStore")

    return product_start
