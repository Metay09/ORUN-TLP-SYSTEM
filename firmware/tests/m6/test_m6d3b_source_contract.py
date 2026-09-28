#!/usr/bin/env python3
"""M6D3B scope contract: flash owner exists without product activation."""
from pathlib import Path

root = Path(__file__).resolve().parents[2]
main = (root / "src" / "main.cpp").read_text()
gate_h = (root / "include" / "flash_mutation_gate.h").read_text()
store_h = (root / "include" / "geofence_store.h").read_text()

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

print("M6D3B source ownership/activation contract: PASS")
