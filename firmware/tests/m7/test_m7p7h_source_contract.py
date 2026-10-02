#!/usr/bin/env python3
from pathlib import Path

root = Path(__file__).resolve().parents[3]
main = (root / "firmware/src/main.cpp").read_text(encoding="utf-8")
request_h = (root / "firmware/include/application_request.h").read_text(encoding="utf-8")
status_h = (root / "firmware/include/application_status.h").read_text(encoding="utf-8")
status_runtime = (root / "firmware/src/application_status_runtime.cpp").read_text(encoding="utf-8")
usb_adapter = (root / "firmware/src/usb_application_adapter.cpp").read_text(encoding="utf-8")
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

# Bounded response shape and access-context seam must exist centrally.
assert "union ApplicationResponsePayload" in request_h
assert "ApplicationAccessContext" in request_h
assert "kGetDeviceStatus" in request_h
assert "kGetTrackingStatus" in request_h
assert "kGetGeofenceStatus" in request_h
assert "kGetStorageStatus" in request_h
assert "kApplicationSurfaceRevision" in status_h
assert "relay_forwarding_applied" in status_h
assert "uptime_ms_mod32" in status_h
assert "uint8_t populated;" in status_h
assert "status_snapshot_->populated == 0" in (
    root / "firmware/src/application_request.cpp"
).read_text(encoding="utf-8")
assert "radio_manager.relayForwardingEnabled()" in main

# Freeze additive BLE message identities for M7P7H.
for needle in (
    "kGetDeviceStatusRequest = 0x02",
    "kGetTrackingStatusRequest = 0x03",
    "kGetGeofenceStatusRequest = 0x04",
    "kGetStorageStatusRequest = 0x05",
    "kGetDeviceStatusResponse = 0x82",
    "kGetTrackingStatusResponse = 0x83",
    "kGetGeofenceStatusResponse = 0x84",
    "kGetStorageStatusResponse = 0x85",
):
    assert needle in ble_h, needle

# Routine status assembly must stay O(1) and must not copy the full geofence
# geometry merely to answer a status query.
for forbidden in (
    "history.newest(",
    "history.backlogCount(",
    "geofence_store.currentSnapshot(",
):
    assert forbidden not in status_runtime, forbidden
assert "history.count()" in status_runtime
assert "geofence_store.areaCount()" in status_runtime
assert "geofence_store.totalVertexCount()" in status_runtime

# main is composition only: concrete snapshot builder + USB adapter.
assert "buildApplicationStatusSnapshot(" in main
assert "parseUsbApplicationQuery(" in main
assert "printUsbApplicationResponse(" in main

# Status assembly must not become idle-loop work. USB refreshes only when an
# APP query is submitted; BLE refreshes only after loop atomically consumes an
# ingress frame from the callback handoff.
assert "refreshApplicationStatusSnapshot(orun_tlp::monotonic::nowMs())" in main
poll_start = main.index("void pollBleApplicationRuntime(")
poll_end = main.index("enum class AccelerometerDiagnosticState", poll_start)
ble_runtime = main[poll_start:poll_end]
assert "if (have_ingress)" in ble_runtime
ingress_pos = ble_runtime.index("if (have_ingress)")
refresh_pos = ble_runtime.index("refreshApplicationStatusSnapshot(now)", ingress_pos)
dispatch_pos = ble_runtime.index("ble_application_transport.onFrameReceived(", ingress_pos)
assert ingress_pos < refresh_pos < dispatch_pos

# USB exposes the four read families plus existing GET_CONFIG through the
# adapter, not transport-specific domain logic in main.
for command in (
    "APP CONFIG?",
    "APP DEVICE?",
    "APP TRACKING?",
    "APP GEOFENCE?",
    "APP STORAGE?",
):
    assert command in usb_adapter, command
    assert command not in main, command

# Engineering-only mutation/probe commands must not be promoted into the BLE
# product message enum.
for forbidden in ("ROLE TRACKER", "ACTIVITY START", "FLASH PROBE", "CRYPTO STRESS"):
    assert forbidden not in ble_h, forbidden

print("M7P7H application surface source-contract guards: PASS")
