#!/usr/bin/env python3
"""M6D3B scope contract: flash owner exists without product activation."""
from pathlib import Path
import re

root = Path(__file__).resolve().parents[2]
main = (root / "src" / "main.cpp").read_text()
gate_h = (root / "include" / "flash_mutation_gate.h").read_text()
store_h = (root / "include" / "geofence_store.h").read_text()
store_cpp = (root / "src" / "geofence_store.cpp").read_text()
preflight = (root / "tests" / "m6" / "m6d3b_geofence_preflight.cpp").read_text()
powercut = (root / "tests" / "m6" / "m6d3b_geofence_powercut_probe.cpp").read_text()
platformio = (root / "platformio.ini").read_text()

assert '#include "geofence_store.h"' not in main
assert "orun_tlp::GeofenceStore " not in main
assert ".geofencePort()" not in main
assert "geofencePort()" in gate_h
assert "class GeofenceStore" in store_h

# M6D3B must not smuggle transport/runtime ownership into the persistence
# owner. Comments intentionally name several forbidden owners to document the
# boundary, so inspect C++ code after removing comments instead of doing a raw
# substring scan.
def strip_cpp_comments(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.DOTALL)
    return re.sub(r"//.*?$", "", text, flags=re.MULTILINE)


def contains_symbol(text: str, symbol: str) -> bool:
    return re.search(rf"\b{re.escape(symbol)}\b", text) is not None


# Guard the guard: comments must be ignored, while real C++ identifiers must
# be detected. These assertions prevent an escaping typo from silently turning
# the ownership contract into a no-op.
assert not contains_symbol(strip_cpp_comments("/* PositionFlow */ int ok;"), "PositionFlow")
assert not contains_symbol(strip_cpp_comments("int ok; // RadioManager"), "RadioManager")
assert contains_symbol(strip_cpp_comments("PositionFlow* forbidden;"), "PositionFlow")

store_code = strip_cpp_comments(store_h + "\n" + store_cpp)
for forbidden in (
    "GnssManager",
    "PositionFlow",
    "RadioManager",
    "GeofenceConfirmationCoordinator",
    "Bluefruit",
):
    assert not contains_symbol(store_code, forbidden), forbidden

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

powercut_env = platformio.split(
    "[env:rak4630_m6d3b_geofence_powercut]", 1
)[1]
assert "+<geofence_store.cpp>" in powercut_env
assert "+<nrf_geofence_flash.cpp>" in powercut_env
assert "+<../tests/m6/m6d3b_geofence_powercut_probe.cpp>" in powercut_env
assert "geofence_incarnation_source.cpp" not in powercut_env
assert "pre:scripts/check_storage_layout.py" in powercut_env

# Power-cut fixture is explicit-command only. Boot must recover existing state
# without manufacturing a new incarnation or silently starting a mutation.
assert 'GeofenceStore geofence_store(cut_backend, nullptr)' in powercut
assert '"CUT_BODY"' in powercut
assert 'cut_backend.arm();' in powercut
assert 'CUT POWER NOW' in powercut
assert 'requestReplace' in powercut and 'requestClear' in powercut

print("M6D3B source ownership/activation contract: PASS")
