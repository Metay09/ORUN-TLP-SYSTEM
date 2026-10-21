#!/usr/bin/env python3
"""SF5K3V pilot cutover safety checks (source only; no RF proof)."""
from pathlib import Path

root = Path(__file__).resolve().parents[3]
app = (root / "firmware/src/main.cpp").read_text()
core = (root / "firmware/src/tracker_product_tx_pilot.cpp").read_text()
cfg = (root / "firmware/platformio.ini").read_text()
release = (root / "firmware/src/custody_ack_release_owner.cpp").read_text()
assert cfg.count("[env:rak4630_sf5k3v_tracker_tx_pilot]") == 1
assert "default_envs = rak4630" in cfg
assert cfg.count("-DORUN_SF5K3V_TRACKER_TX_PILOT") == 1
assert "#ifdef ORUN_SF5K3V_TRACKER_TX_PILOT" in app
assert "tracker_product_tx.update(pilot_allowed)" in app
assert app.index("history_store_forward.update(") < app.index(
    "tracker_product_tx.update(pilot_allowed)")
assert app.index("product_records.poll();") < app.index(
    "tracker_product_tx.update(pilot_allowed)")
for name in [
    "!positions.pending()", "!custody_ack_release.pending()",
    "SecurityState::kProvisioned", "!security_store.busy()",
    "radio_manager.role() == orun_tlp::NodeRole::kTracker",
]:
    assert name in app[app.index("const bool pilot_allowed"):], name
assert core.index("findCachedExact(identity_, cached)") < core.index(
    "crypto_.protectNextPeriodic(")
# Waiting-cache is deliberately handled first on subsequent loop passes;
# it must NOT be interpreted as an unconditional send permission.
assert "submitExactCache(sealed_)" in core
assert "takeCacheOutcome(outcome)" in core
assert "if (phase_ != Phase::kReady) return Event::kNone;" in core
assert core.index("takeCacheOutcome(outcome)") < core.index(
    "sendProductSecurePacket(exact.object, exact.object_size)")
assert "custodyCandidateMatchesRetainedExact(exact)" in core
assert "Phase::kDone;" in core and "Phase::kBlocked;" in core
assert "custody_ack_release.submit(ack.payload, ack.size)" in app
assert "verifier_.verifyFromStore(ack,length,product_,proof)" in release
assert "requestPrepare(" not in core
assert "requestRelease(" not in core
print("SF5K3V pilot safety source checks: PASS (source only)")
