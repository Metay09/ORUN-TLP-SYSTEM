#!/usr/bin/env python3
"""SF5K3AC: no field-writer composition, strict source/issuer ownership."""
from pathlib import Path

root = Path(__file__).resolve().parents[3]
app = (root / "firmware/src/main.cpp").read_text()
hdr = (root / "firmware/include/gateway_signed_grant_append_transaction.h").read_text()
cpp = (root / "firmware/src/gateway_signed_grant_append_transaction.cpp").read_text()
preflight = (root / "firmware/src/gateway_signed_grant_append_preflight.cpp").read_text()
reader = (root / "firmware/include/gateway_grant_snapshot_reader.h").read_text()
test = (root / "firmware/tests/sf5/test_sf5k3ac_signed_append_transaction.cpp").read_text()
suite = (root / "firmware/tests/run_host_tests.sh").read_text()

assert "GatewaySignedGrantAppendTransaction" not in app
assert "ORUN_SF5K3AC_TEST_ONLY_TRANSACTION" in hdr
assert "ORUN_SF5K3AC_TEST_ONLY_TRANSACTION" in cpp
assert "friend class GatewaySignedGrantAppendTransaction;" in reader
assert "kIssuerAuthorityMismatch" in preflight
assert "&current.verifier_ != &candidate.verifier_" in preflight
assert "GatewaySignedGrantAppendPreflight::inspect(" in cpp
assert "current.denyForCurrentBoot();" in cpp
assert "flash_.erasePage(destination_)" in cpp
assert "flash_.program(dest_base + offset_" in cpp
assert "flash_.program(dest_base + fmt::kCommitOffset" in cpp
assert "phase_ == Phase::kCommit && !waiting_" in cpp
assert "verifyPreCommit()" in cpp
assert "flash_.pollPending()" in cpp
assert "kCommittedAwaitingAnchor" in cpp
assert "secureWipe(candidate_" in cpp
assert "testTornPowerCutRefusesBoot" in test
assert "testRevokedAnchorDuringPrepare" in test
assert "testAnchorChangesAfterPrecommit" in test
assert "testAsyncCompletionNoDoubleSubmission" in test
assert "run_sf5k3ac_signed_append_transaction_host.sh" in suite
assert "-DORUN_SF5K3AC_TEST_ONLY_TRANSACTION=1" in (
    root / "tools/ci/run_sf5k3ac_signed_append_transaction_host.sh"
).read_text()
print("SF5K3AC test-only/commit-last/issuer/anchor source contract: PASS")
