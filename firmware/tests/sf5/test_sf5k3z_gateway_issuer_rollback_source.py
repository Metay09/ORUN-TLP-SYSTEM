#!/usr/bin/env python3
"""SF5K3Z: host proof/rollback adapter never silently enables Gateway."""
from pathlib import Path

root = Path(__file__).resolve().parents[3]
app = (root / "firmware/src/main.cpp").read_text()
guard = (root / "firmware/src/gateway_anchored_grant_source.cpp").read_text()
api = (root / "firmware/include/gateway_anchored_grant_source.h").read_text()
reader = (root / "firmware/src/gateway_grant_snapshot_reader.cpp").read_text()
test = (root / "firmware/tests/sf5/test_sf5k3z_gateway_issuer_rollback.cpp").read_text()

assert "GatewayAnchoredGrantSource" not in app
assert "GatewayGrantRevisionAnchor" not in app
assert "GatewayGrantSnapshotReader" not in app
assert "GatewayGrantRevisionAnchor" in api
assert "virtual bool currentRevision(" in api
assert "authoritative == snapshot_.activeGeneration()" in guard
assert "verifiedForLocalGateway(local)" in guard
assert "wipe(candidate_key, sizeof(candidate_key))" in guard
assert "snapshot_.activeGrant(" in guard
assert "verifier_.verify(" in reader
assert "EVP_DigestVerify(" in test and "EVP_DigestSign(" in test
assert "EVP_PKEY_ED25519" in test
assert "testBootGuardIntegration()" in (
    root / "firmware/tests/sf5/test_sf5k3y_gateway_grant_reader.cpp"
).read_text()
for forbidden in ["erasePage(", "program(", "Radio.Send(", "commitCredential(",
                  "gateway_id_ = 0", "SecurityStore", "K_root"]:
    assert forbidden not in guard, forbidden
print("SF5K3Z no-live-authority/exact-checkpoint/source contract: PASS")
