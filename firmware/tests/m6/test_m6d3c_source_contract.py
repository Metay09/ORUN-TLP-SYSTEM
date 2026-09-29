#!/usr/bin/env python3
"""M6D3C production activation contract: durable snapshot -> M6D2 runtime only."""
from pathlib import Path
import re

root = Path(__file__).resolve().parents[2]
main = (root / "src" / "main.cpp").read_text()
provider_h = (root / "include" / "geofence_runtime_provider.h").read_text()
provider_cpp = (root / "src" / "geofence_runtime_provider.cpp").read_text()

# Production now intentionally owns exactly one durable GeofenceStore and uses
# the existing shared flash arbiter. M6D3C adds no second persistence owner.
assert main.count("orun_tlp::GeofenceStore geofence_store") == 1
assert "storage_flash_gate.geofencePort()" in main
assert main.count("orun_tlp::NrfGeofenceIncarnationSource geofence_incarnation_source") == 1

# M6D3B physically qualified GeofenceStore::begin() against the 4-KiB loop
# task. Do not reintroduce a record-sized setup() local around that call.
assert main.count("orun_tlp::geofence_format::Snapshot geofence_boot_snapshot") == 1
assert "currentSnapshot(geofence_boot_snapshot)" in main

# Recovery and runtime activation must happen before Bluefruit enables
# SoftDevice. This keeps the already-qualified synchronous boot path intact.
begin_pos = main.index("geofence_store.begin()")
ble_pos = main.index("ble_ready = Bluefruit.begin()")
assert begin_pos < ble_pos

# The runtime bridge consumes a recovered snapshot only. No production
# geofence mutation API is activated in this slice.
assert "geofence_store.currentSnapshot(geofence_boot_snapshot)" in main
assert "applyGeofenceSnapshotToRuntime" in main
assert "geofence_store.requestReplace" not in main
assert "geofence_store.requestClear" not in main

# Token authority and semantic readability must remain separate. Production
# logs token state but does not gate currentSnapshot/configure on VALID.
snapshot_pos = main.index("geofence_store.currentSnapshot(geofence_snapshot)")
apply_pos = main.index("applyGeofenceSnapshotToRuntime")
token_pos = main.index("geofence_store.tokenState()", apply_pos)
assert snapshot_pos < apply_pos < token_pos

# The provider is a narrow bridge, not a new owner of GNSS, radio, BLE,
# persistence or transport.
provider_code = provider_h + "\n" + provider_cpp
for forbidden in (
    "GnssManager",
    "RadioManager",
    "Bluefruit",
    "FlashBackend",
    "GeofenceStore",
    "PositionFlow",
    "ApplicationRequest",
):
    assert re.search(rf"\b{re.escape(forbidden)}\b", provider_code) is None, forbidden

# Durable CLEAR and CONFIGURED are the only provider inputs; malformed
# structure rejects without inventing runtime authority.
assert "ResourceState::kClear" in provider_cpp
assert "ResourceState::kConfigured" in provider_cpp
assert "GeofenceRuntimeApplyResult::kRejected" in provider_cpp
assert "coordinator.clear()" in provider_cpp
assert "coordinator.configure(" in provider_cpp

print("M6D3C durable geofence runtime activation contract: PASS")
