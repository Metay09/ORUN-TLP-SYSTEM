#!/usr/bin/env python3
from pathlib import Path

root = Path(__file__).resolve().parents[3]
main = (root / "firmware/src/main.cpp").read_text(encoding="utf-8")
request_h = (root / "firmware/include/application_request.h").read_text(encoding="utf-8")
status_h = (root / "firmware/include/application_status.h").read_text(encoding="utf-8")
ble_h = (root / "firmware/include/ble_application_transport.h").read_text(encoding="utf-8")

# ApplicationRequestService must stay independent of concrete drivers/Arduino.
for forbidden in (
    "gnss_manager.h",
    "radio_manager.h",
    "Arduino.h",
    "bluefruit.h",
    "geofence_store.h",
    "history_store.h",
):
    assert forbidden not in request_h, forbidden

# Bounded POD response shape and access-context seam must exist centrally.
assert "union ApplicationResponsePayload" in request_h
assert "ApplicationAccessContext" in request_h
assert "kGetDeviceStatus" in request_h
assert "kGetTrackingStatus" in request_h
assert "kGetGeofenceStatus" in request_h
assert "kGetStorageStatus" in request_h
assert "kApplicationSurfaceRevision" in status_h

# Freeze additive BLE message identities for M7P7H.
for text in (
    "kGetDeviceStatusRequest = 0x02",
    "kGetTrackingStatusRequest = 0x03",
    "kGetGeofenceStatusRequest = 0x04",
    "kGetStorageStatusRequest = 0x05",
    "kGetDeviceStatusResponse = 0x82",
    "kGetTrackingStatusResponse = 0x83",
    "kGetGeofenceStatusResponse = 0x84",
    "kGetStorageStatusResponse = 0x85",
):
    assert text in ble_h, text

# Routine status assembly must not invoke O(N) history scans or copy the full
# geofence geometry. Limit the check to the actual refresh function body.
start = main.index("void refreshApplicationStatusSnapshot(")
brace = main.index("{", start)
depth = 0
end = None
for i in range(brace, len(main)):
    if main[i] == "{":
        depth += 1
    elif main[i] == "}":
        depth -= 1
        if depth == 0:
            end = i + 1
            break
assert end is not None
refresh = main[start:end]
for forbidden in ("history.newest(", "history.backlogCount(", "geofence_store.currentSnapshot("):
    assert forbidden not in refresh, forbidden
assert "history.count()" in refresh
assert "geofence_store.areaCount()" in refresh
assert "geofence_store.totalVertexCount()" in refresh

# USB exposes only the four read families plus the existing GET_CONFIG seam.
for command in (
    'APP CONFIG?',
    'APP DEVICE?',
    'APP TRACKING?',
    'APP GEOFENCE?',
    'APP STORAGE?',
):
    assert command in main, command

# Engineering-only mutation/probe commands must not be promoted into the BLE
# application message enum.
for forbidden in ("ROLE TRACKER", "ACTIVITY START", "FLASH PROBE", "CRYPTO STRESS"):
    assert forbidden not in ble_h, forbidden

print("M7P7H application surface source-contract guards: PASS")
