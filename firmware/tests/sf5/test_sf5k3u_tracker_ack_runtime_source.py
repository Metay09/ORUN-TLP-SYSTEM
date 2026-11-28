#!/usr/bin/env python3
"""Current authenticated Tracker ACK runtime contract."""
from pathlib import Path

repo = Path(__file__).resolve().parents[3]

src = (repo / "firmware/src/main.cpp").read_text()
owner = (repo / "firmware/src/kq05_tracker_batch_release_owner.cpp").read_text()
radio = (repo / "firmware/src/radio_manager.cpp").read_text()

assert '#include "custody_ack_release_owner.h"' not in src
assert "custody_ack_release.submit(" not in src
assert "custody_ack_release.service()" not in src

assert src.count("Kq05TrackerBatchReleaseOwner batch_ack_release(") == 1
assert src.count("batch_ack_release.submit(") == 1
assert src.count("batch_ack_release.serviceAndNotify(") == 1

assert src.index("product_records.poll();") < src.index("batch_ack_release.serviceAndNotify(")
assert src.index("radio_manager.update(tracking_enabled") < src.index("batch_ack_release.submit(")

guard = src.split("// SF5K3U: loop-only Tracker ACK intake,")[1].split(
    "// SF3 device runtime executes", 1
)[0]

for phrase in (
    "radio_manager.role() == orun_tlp::NodeRole::kTracker",
    "!custodyReleasePending()",
    "radio_manager.takeCustodyAckFrame(&ack)",
    "ble_ready",
    "product_records.mounted()",
    "product_records.store().ready()",
    "security_store.ready()",
    "SecurityState::kProvisioned",
    "!security_store.busy()",
    "batch_ack_release.submit(ack.payload, ack.size)",
    "memset(ack.payload, 0, sizeof(ack.payload))",
):
    assert phrase in guard, phrase

assert "CustodyAckReleaseOwner::Service::kDurablyReleased" not in src
api = (repo / "firmware/include/kq05_tracker_batch_release_owner.h").read_text()
assert "if (result == Service::kApplied) notify();" in api

assert "product_records.store().requestRelease(" not in src
assert "sendAuthorizedCustodyAck(" not in src

assert "verifier_.verifyFromStore(frame,length,records_,verified)" in owner
assert "records_.requestAuthenticatedCustodyRelease(exact)" in owner
assert "currentGrant()" in owner

assert "custody_ack::decodeHeader" not in radio
assert "kq05_gateway_batch_ack::decode" in radio

print("ORUN-305 SINGLE TRACKER ACK RUNTIME CONTRACT: PASS")
