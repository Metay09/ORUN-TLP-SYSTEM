#!/usr/bin/env python3
"""SF5K3AA isolate the genuine target CC310 verifier from live product."""
from pathlib import Path

root = Path(__file__).resolve().parents[3]
main = (root / "firmware/src/main.cpp").read_text()
h = (root / "firmware/include/gateway_ed25519_cc310_verifier.h").read_text()
cpp = (root / "firmware/src/gateway_ed25519_cc310_verifier.cpp").read_text()
probe = (root / "firmware/tests/sf5/sf5k3aa_ed25519_probe.cpp").read_text()
ini = (root / "firmware/platformio.ini").read_text()
host = (root / "firmware/tests/sf5/test_sf5k3aa_cc310_ed25519_adapter.cpp").read_text()

assert "default_envs = rak4630" in ini
assert ini.count("[env:rak4630_sf5k3aa_ed25519_probe]") == 1
assert ini.count("-DORUN_SF5K3AA_TARGET_CRYPTO_PROBE=1") == 1
assert "GatewayEd25519Cc310Verifier" not in main
assert "GatewayProductStartupGuard" not in main
assert "ORUN_SF5K3AA_TARGET_CRYPTO_PROBE" in h
assert "ORUN_SF5K3AA_TARGET_CRYPTO_PROBE" in cpp
assert "CRYS_ECEDW_Verify(" in cpp
assert "CRYS_ECEDW_TempBuff_t context" in cpp
assert "const uint8_t (&trusted_issuer_public_key)[32]" in h
assert "return rc == CRYS_OK;" in cpp
assert "secureWipe(message, sizeof(message));" in cpp
assert "secureWipe(&context, sizeof(context));" in cpp
assert "nRFCrypto.begin()" in probe and "nRFCrypto.end()" in probe
assert "EVP_DigestVerify(" in host
assert "GatewayEd25519Cc310Verifier" in host
for prohibited in ("Radio.Send(", "sd_flash_", "erasePage(", "commitCredential("):
    assert prohibited not in cpp and prohibited not in probe
print("SF5K3AA Ed25519 target-only/no-activation source contract: PASS")
