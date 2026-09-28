#!/usr/bin/env python3
"""M6D3B scope contract: flash owner exists without product activation."""
from pathlib import Path

root = Path(__file__).resolve().parents[2]
main = (root / "src" / "main.cpp").read_text()
gate_h = (root / "include" / "flash_mutation_gate.h").read_text()
store_h = (root / "include" / "geofence_store.h").read_text()
preflight = (root / "tests" / "m6" / "m6d3b_geofence_preflight.cpp").read_text()
platformio = (root / "platformio.ini").read_text()

assert "GeofenceStore" not in main
assert ".geofencePort()" not in main
assert "geofencePort()" in gate_h
assert "class GeofenceStore" in store_h

# M6D3B must not smuggle transport/runtime ownership into the persistence owner.
for forbidden in (
    "GnssManager",
    "PositionFlow",
    "RadioManager",
    "GeofenceConfirmationCoordinator",
    "Bluefruit",
):
    assert forbidden not in store_h, forbidden

# The physical preflight must remain structurally read-only: no backend with
# mutator methods and no Nordic flash primitive may even be linked by its env.
for forbidden in (
    "NrfGeofenceFlash",
    "sd_flash_write",
    "sd_flash_page_erase",
    ".program(",
    ".erasePage(",
):
    assert forbidden not in preflight, forbidden

preflight_env = platformio.split(
    "[env:rak4630_m6d3b_geofence_preflight]", 1
)[1].split("[env:rak4630_m6d3b_geofence_qual]", 1)[0]
assert "nrf_geofence_flash.cpp" not in preflight_env
assert "geofence_store.cpp" not in preflight_env
assert "extra_scripts =" in preflight_env

qual_env = platformio.split("[env:rak4630_m6d3b_geofence_qual]", 1)[1]
assert "+<geofence_store.cpp>" in qual_env
assert "+<nrf_geofence_flash.cpp>" in qual_env
assert "pre:scripts/check_storage_layout.py" in qual_env

print("M6D3B source ownership/activation contract: PASS")
