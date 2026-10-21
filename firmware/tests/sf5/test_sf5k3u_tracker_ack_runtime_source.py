#!/usr/bin/env python3
"""Verify actual loop ACK intake stays behind the existing HMAC/flash owner."""
from pathlib import Path

repo = Path(__file__).resolve().parents[3]
src = (repo / "firmware/src/main.cpp").read_text()
owner = (repo / "firmware/src/custody_ack_release_owner.cpp").read_text()
radio = (repo / "firmware/src/radio_manager.cpp").read_text()

assert '#include "custody_ack_release_owner.h"' in src
assert src.count("CustodyAckReleaseOwner custody_ack_release(") == 1
assert src.count("custody_ack_release.submit(") == 1
assert src.count("custody_ack_release.service()") == 1
assert src.index("product_records.poll();") < src.index("custody_ack_release.service()")
assert src.index("radio_manager.update(tracking_enabled") < src.index("custody_ack_release.submit(")
guard = src.split("// SF5K3U: loop-only Tracker ACK intake,")[1].split(
    "// SF3 device runtime executes", 1)[0]
for phrase in (
    "radio_manager.role() == orun_tlp::NodeRole::kTracker",
    "!custody_ack_release.pending()",
    "radio_manager.takeCustodyAckFrame(&ack)",
    "ble_ready",
    "product_records.mounted()",
    "product_records.store().ready()",
    "security_store.ready()",
    "SecurityState::kProvisioned",
    "!security_store.busy()",
    "custody_ack_release.submit(ack.payload, ack.size)",
    "memset(ack.payload, 0, sizeof(ack.payload))",
):
    assert phrase in guard, phrase
assert "CustodyAckReleaseOwner::Service::kDurablyReleased" in src
assert "product_records.store().requestRelease(" not in src
assert "sendProductSecurePacket(" not in src
assert "sendAuthorizedCustodyAck(" not in src
assert "GatewayCustodyAuthorizedIngress" not in src
assert "verifier_.verifyFromStore(ack,length,product_,proof)" in owner
assert "product_.requestAuthenticatedCustodyRelease(selected)" in owner
assert "network_.role() != NodeRole::kTracker" in radio
print("SF5K3U Tracker RX -> authenticated release runtime: PASS (source contract)")
