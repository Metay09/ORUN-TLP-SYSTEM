#!/usr/bin/env python3
"""SF5K3Y: no unauthorized enrollment or live Gateway activation."""
from pathlib import Path

root = Path(__file__).resolve().parents[3]
reader = (root / "firmware/src/gateway_grant_snapshot_reader.cpp").read_text()
header = (root / "firmware/include/gateway_grant_snapshot_reader.h").read_text()
app = (root / "firmware/src/main.cpp").read_text()
suite = (root / "firmware/tests/run_host_tests.sh").read_text()

assert "GatewayGrantSnapshotReader" not in app
assert "GatewayProductStartupGuard" not in app
assert "GatewayDurableGrantSource : public GatewayCustodyGrantProvider" in (
    root / "firmware/include/gateway_product_startup_guard.h").read_text()
assert "kMagic = 0x4F474B31U" in header  # OGK1, different from ORS1
assert "GatewayEnrollmentProofVerifier" in header
assert "kGrantSlots = 32U" in header
assert "tracker_security_store_mounted" in reader
assert "gateway_security_owner" in reader
assert "verifier_.verify(" in reader
assert "product_region::" not in reader
assert "GatewayProductStartupGuard" not in reader
assert "if (status_ != Status::kUnopened) return status_;" in reader
assert "if (generation == selected_generation)" in reader
assert "denyForCurrentBoot()" in reader
for mutator in ("erasePage(", "program(", "requestPrepare(", "commitCredential(",
                "beginV3PolicyMutation(", "sd_flash_", "Radio.Send("):
    assert mutator not in reader, mutator
assert "run_sf5k3y_gateway_grant_reader_host.sh" in suite
print("SF5K3Y no-production-verifier/no-write/no-activation contract: PASS")
