#!/usr/bin/env python3
"""SF5K3X candidate does not activate or weaken the existing product."""
from pathlib import Path

root = Path(__file__).resolve().parents[3]
app = (root / "firmware/src/main.cpp").read_text()
gate_h = (root / "firmware/include/gateway_product_startup_guard.h").read_text()
gate_cpp = (root / "firmware/src/gateway_product_startup_guard.cpp").read_text()
gateway_h = (root / "firmware/include/gateway_product_custody_runtime.h").read_text()
ini = (root / "firmware/platformio.ini").read_text()

assert "default_envs = rak4630" in ini
assert "GatewayProductStartupGuard" not in app
assert "GatewayDurableGrantSource" not in app
assert "product_records.begin(device_identity.legacyUint64()," in app
assert "Jobs{false, true}" in app
assert "GatewayDurableGrantSource : public GatewayCustodyGrantProvider" in gate_h
assert "verifiedForLocalGateway" in gate_h
assert "verifiedForLocalGateway(gateway_id_)" in gate_cpp
assert "observation_store_mounted" in gate_cpp
assert "flash_.hasUnreconciledMutation()" in gate_cpp
assert "product_region::takeCensus" in gate_cpp
assert "product_region::mountProductCustodyDecision" in gate_cpp
assert "custody_.begin()" in gate_cpp
assert "if (state_ != State::kNotChecked) return state_;" in gate_cpp
assert "updateGuarded(const GatewayProductStartupGuard& gate" in gateway_h
assert "gate.mayServe(" in gateway_h
for operation in ["erasePage(", "program(", "requestPrepare(", "commitCredential(",
                  "commitCredential", "beginV3PolicyMutation("]:
    assert operation not in gate_cpp, operation
print("SF5K3X no-activation/one-owner/zero-erase source contract: PASS")
