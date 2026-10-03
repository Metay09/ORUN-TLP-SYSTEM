#!/usr/bin/env python3
from pathlib import Path

root = Path(__file__).resolve().parents[3]
main = (root / "firmware/src/main.cpp").read_text(encoding="utf-8")
owner_h = (root / "firmware/include/location_owner.h").read_text(encoding="utf-8")
mono_h = (root / "firmware/include/monotonic_time.h").read_text(encoding="utf-8")
mono_cpp = (root / "firmware/src/monotonic_time.cpp").read_text(encoding="utf-8")
request_h = (root / "firmware/include/application_request.h").read_text(encoding="utf-8")
ble_h = (root / "firmware/include/ble_application_transport.h").read_text(encoding="utf-8")
usb_adapter = (root / "firmware/src/usb_application_adapter.cpp").read_text(encoding="utf-8")

for forbidden in (
    "gnss_manager.h",
    "history_store.h",
    "radio_manager.h",
    "geofence_",
    "Arduino.h",
    "bluefruit.h",
):
    assert forbidden not in owner_h, forbidden

assert "enum class LocationSource" in owner_h
assert "kGnss = 1" in owner_h
assert "uint64_t observed_monotonic_ms" in owner_h
assert "class LocationOwner" in owner_h
assert "bool accept(const AcceptedLocation& candidate)" in owner_h
assert "void clear(" not in owner_h

assert "uint64_t update64(" in mono_h
assert "uint32_t update(" in mono_h
assert "uint64_t nowMs64()" in mono_h
assert mono_cpp.count("TickMillis clock;") == 1
assert "uint64_t updateClock()" in mono_cpp
assert "return static_cast<uint32_t>(updateClock());" in mono_cpp
assert "return updateClock();" in mono_cpp

take = main.index("gnss_manager.takeFreshFixForTransmission(&fix)")
publish = main.index("location_owner.accept(accepted_location)", take)
geofence = main.index("processGeofenceAcceptedFix(fix", take)
normal_only = main.index("if (!confirmation_fix_expected)", take)
assert take < publish < geofence < normal_only
segment = main[take:geofence]
assert "monotonic::nowMs64()" in segment
assert "extendRecentMonotonicMs" in segment
assert "LocationSource::kGnss" in segment
assert "accepted_location.altitude_valid = true" in segment
assert "kPositionFlagValidUtcTime" in segment

for text in (request_h, ble_h):
    assert "GetLocation" not in text
assert "APP LOCATION?" not in usb_adapter

print("M7P7I accepted Location owner source-contract guards: PASS")
