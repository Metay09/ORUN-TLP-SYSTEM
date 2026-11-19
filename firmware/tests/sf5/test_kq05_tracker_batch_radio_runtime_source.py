#!/usr/bin/env python3
"""Source integration contract: opt-in 0x0A Tracker radio intake never
breaks default 0x05/0x09 paths or starves live position scheduling.
This is not an image/build/real-RF test."""
from pathlib import Path

repo = Path(__file__).resolve().parents[3]
main = (repo / "firmware/src/main.cpp").read_text()
adapter = (repo / "firmware/src/kq05_tracker_batch_radio_runtime.cpp").read_text()
release = (repo / "firmware/src/kq05_tracker_batch_release_owner.cpp").read_text()
radio = (repo / "firmware/src/radio_manager.cpp").read_text()
config = (repo / "firmware/platformio.ini").read_text()

flag = "ORUN_KQ05_EXPERIMENTAL_GATEWAY_BATCH_RF"
assert "-D" + flag not in config, "experimental group RX must be disabled by default"
assert main.count("Kq05TrackerBatchRadioRuntime tracker_gateway_batch_runtime(") == 1
assert main.count("Kq05TrackerBatchReleaseOwner tracker_gateway_batch_release(") == 1
assert main.count("tracker_gateway_batch_runtime.receive(") == 1
assert main.count("tracker_gateway_batch_release.service()") == 1
assert main.count("custody_ack_release.service()") == 1
assert main.count("custody_ack_release.submit(") == 1
assert main.count("kq05_batch_release.submit(") == 1
assert main.count("kq05_batch_release.service()") == 1
assert main.index("product_records.poll();") < main.index(
    "tracker_gateway_batch_release.service()"
)
assert main.index("radio_manager.update(tracking_enabled") < main.index(
    "tracker_gateway_batch_runtime.receive("
)
assert main.index("tracker_gateway_batch_runtime.receive(") < main.index(
    "radio_manager.takeCustodyAckFrame(&ack)"
)
assert "#ifdef " + flag in main
assert "tracker_gateway_batch_release.pending()" in main
assert "!positions.pending()" in main.split(
    "tracker_gateway_batch_release.pending() &&", 1
)[1].split("tracker_gateway_batch_release.service()", 1)[0]
assert "!kq05_batch_release.pending()" in main.split(
    "tracker_gateway_batch_runtime.receive(", 1
)[0]
assert "!tracker_gateway_batch_release.pending()" in main.split(
    "radio_manager.takeCustodyAckFrame(&ack)", 1
)[0]
pilot = main.split("const bool pilot_allowed =", 1)[1].split(";", 1)[0]
assert "!tracker_gateway_batch_release.pending()" not in pilot, (
    "batch ACK wait must not become a live-position TX gate"
)
assert "tracker_gateway_batch_runtime.receive(" not in radio, (
    "radio callback must NEVER issue a release"
)
for needle in (
    "release_.submit(raw.payload,raw.size)",
    "radio_.peekKq05GatewayBatchAckFrame(&raw)",
    "radio_.discardKq05GatewayBatchAckFrame()",
    "kMaxTemporaryAttempts = 12U",
):
    body = adapter if "kMaxTemporaryAttempts" not in needle else (
        repo / "firmware/include/kq05_tracker_batch_radio_runtime.h"
    ).read_text()
    assert needle in body, needle
assert "verifier_.verifyFromStore(frame,length,records_,verified)" in release
assert "records_.requestAuthenticatedCustodyRelease(exact)" in release
assert "kq05_gateway_batch_ack::decode(payload,size,header)" in radio
print("KQ05 0x0A Tracker main loop / 0x05 / 0x09 / LIVE priority source contract: PASS")
