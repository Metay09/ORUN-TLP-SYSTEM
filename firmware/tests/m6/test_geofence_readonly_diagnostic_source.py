#!/usr/bin/env python3
"""Fail-closed source contract for physical USB geofence A/B read-only diagnosis.

This test checks the new diagnostic's command routing, flash bounds and absence
of mutator calls. Run it alongside the regular firmware host tests and a real
rak4630_orun PlatformIO build. It is not a substitute for those tests.
"""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
main = (ROOT / "firmware/src/main.cpp").read_text(encoding="utf-8")
begin = main.index("void printUsbGeofencePageDiagnostics() {")
end = main.index("void startUsbApplicationQuery(", begin)
diagnostic = main[begin:end]
route = main.index('isActivityCommand("APP FENCE DIAG?", 15)')
parser = main.index("const auto fence_command = orun_tlp::parseUsbFenceCommand(")

assert route < parser, "read-only query must precede the APP FENCE mutation parser"
assert "printUsbGeofencePageDiagnostics();" in main[route:parser]
assert 'static orun_tlp::geofence_format::PageInspection inspection' in diagnostic
assert "geofence_store.busy()" in diagnostic
assert "kGeofenceRegionPages" in diagnostic
assert "kGeofenceRegionStart + p * kPageSize" in diagnostic
assert "i < kPageSize" in diagnostic
assert "kRecordSize" in diagnostic
assert "inspectPage(" in diagnostic
assert "journal_format::crc32(" in diagnostic
assert "read_only=yes" in diagnostic
assert "incarnation_same_as_A" in diagnostic

for forbidden in (
    "erasePage(", "program(", "sd_flash_write(", "sd_flash_page_erase(",
    "requestReplace(", "requestClear(", "startMutation(", "memcpy(",
):
    assert forbidden not in diagnostic, f"forbidden mutation in diagnostic: {forbidden}"

print("ORUN_GEOFENCE_READONLY_USB_SOURCE_PASS")
