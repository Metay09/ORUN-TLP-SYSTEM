#!/usr/bin/env python3
"""SF5K3AB is an informational preflight, never enrollment authority."""
from pathlib import Path

root = Path(__file__).resolve().parents[3]
main = (root / "firmware/src/main.cpp").read_text()
api = (root / "firmware/include/gateway_signed_grant_append_preflight.h").read_text()
impl = (root / "firmware/src/gateway_signed_grant_append_preflight.cpp").read_text()
snapshot = (root / "firmware/include/gateway_grant_snapshot_reader.h").read_text()
test = (root / "firmware/tests/sf5/test_sf5k3ab_signed_append_preflight.cpp").read_text()
suite = (root / "firmware/tests/run_host_tests.sh").read_text()
assert "GatewaySignedGrantAppendPreflight" not in main
assert "GatewaySignedGrantAppendPreflight" in api
assert "kEligibleForSeparateCommitReview" in api
assert "friend class GatewaySignedGrantAppendPreflight" in snapshot
assert "anchor.currentRevision(gateway, revision)" in impl
assert "anchor.currentRevision(gateway, final_revision)" in impl
assert "requested != generation + 1U" in impl
assert "memcmp(before, after, fmt::kGrantBytes) != 0" in impl
assert "kNoNewTracker" in impl and "kExistingGrantChanged" in impl
assert "GatewayGrantSnapshotReader" in test and "Issuer issuer" in test
assert "run_sf5k3ab_signed_append_preflight_host.sh" in suite
for bad in ("flash_.program(", "erasePage(", "program(", "sd_flash_",
            "commitCredential(", "requestCustody(", "Radio.Send("):
    assert bad not in impl, bad
print("SF5K3AB read-only issuer/rollback/custody source contract: PASS")
