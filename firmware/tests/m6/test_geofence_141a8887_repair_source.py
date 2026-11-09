#!/usr/bin/env python3
"""Fail-closed source contract for one-time 141A8887 geofence repair.
This guard does not replace full host tests and a real production build.
"""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
main = (ROOT / "firmware/src/main.cpp").read_text(encoding="utf-8")
ini = (ROOT / "firmware/platformio.ini").read_text(encoding="utf-8")
script = (ROOT / "tools/orun_geofence_141a8887_repair.sh").read_text(
    encoding="utf-8"
)

start = main.index("bool repairGeofence141A8887OnBoot(uint64_t device_id)")
end = main.index("#endif", start)
repair = main[start:end]
normal_env = ini.split("[env:rak4630_orun]", 1)[1].split("[env:", 1)[0]
service_env = ini.split("[env:rak4630_orun_fence_repair_141a8887]", 1)[1].split(
    "[env:", 1
)[0]

assert "#ifdef ORUN_GEOFENCE_SAFE_REINIT_141A8887" in main[:start]
assert "repairGeofence141A8887OnBoot(device_identity.legacyUint64());" in main
assert main.index("repairGeofence141A8887OnBoot(device_identity.legacyUint64());") < main.index(
    "if (!geofence_store.begin())"
)
assert main.index("if (!geofence_store.begin())") < main.index(
    "ble_ready = Bluefruit.begin();"
)
assert "UINT64_C(0x52E16773141A8887)" in repair
assert "kGeofenceRegionStart == 0x0E5000U" in repair
assert "kGeofenceRegionEnd == 0x0E7000U" in repair
assert "kGeofenceRegionPages == 2U" in repair
assert "PageEvidence::kSupportedCorrupt" in repair
assert "inspection.has_decoded_record" in repair
assert "kPageSize; offset < 2U * kPageSize" in repair
assert "if (byte != 0xFFU)" in repair
assert "dirty_tail" in repair
assert "port.erasePage(0)" in repair
assert repair.count("erasePage(") == 1, "only page A may be erased"
assert "port.program(" not in repair, "baseline must be owned by GeofenceStore"
assert "sd_flash_" not in repair
assert "geofence_store.begin()" not in repair, "store begins by standard setup path"
assert "-DORUN_GEOFENCE_SAFE_REINIT_141A8887=1" in service_env
assert "ORUN_GEOFENCE_SAFE_REINIT_141A8887" not in normal_env
assert "rak4630_orun" in script
assert "rak4630_orun_fence_repair_141a8887" in script
assert "ONAR" in script
assert "karsilastir" in script
assert "verify_fence repaired" in script
assert "verify_fence final" in script
assert "52E16773141A8887" in script
assert "APP FENCE SERVICE?" in script
print("ORUN_GEOFENCE_141A8887_REPAIR_SOURCE_PASS")
