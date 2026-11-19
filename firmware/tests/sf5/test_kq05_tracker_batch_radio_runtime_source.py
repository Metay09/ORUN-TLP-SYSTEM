#!/usr/bin/env python3
"""Source integration contract: 0x0A Tracker intake excludes competing
Backend 0x09 release in unified image and does not starve LIVE.
This is not an image/build/real-RF test."""
from pathlib import Path

repo = Path(__file__).resolve().parents[3]
main = (repo / "firmware/src/main.cpp").read_text()
adapter = (repo / "firmware/src/kq05_tracker_batch_radio_runtime.cpp").read_text()
release = (repo / "firmware/src/kq05_tracker_batch_release_owner.cpp").read_text()
radio = (repo / "firmware/src/radio_manager.cpp").read_text()
config = (repo / "firmware/platformio.ini").read_text()

flag = "ORUN_KQ05_EXPERIMENTAL_GATEWAY_BATCH_RF"
assert "-D" + flag not in config.split("[env:rak4630_kq05_unified_ack_lab]",1)[0], "default images must not enable batch"
assert "-D" + flag in config.split("[env:rak4630_kq05_unified_ack_lab]",1)[1].split("[env:",1)[0], "dedicated lab must opt in"
assert main.count("Kq05TrackerBatchRadioRuntime tracker_gateway_batch_runtime(") == 1
assert main.count("Kq05TrackerBatchReleaseOwner tracker_gateway_batch_release(") == 1
assert main.count("tracker_gateway_batch_runtime.receive(") == 1
assert main.count("tracker_gateway_batch_release.service()") == 1
assert "#ifndef " + flag in main, "legacy single owner must be compile-excluded from unified image"
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
assert "kq05_batch_release.pending()" in main  # older lab only
assert "pilot_allowed && !tracker_gateway_batch_release.pending()" in main
assert "pilot_allowed && !kq05_batch_release.pending()" in main
# Legacy 0x05 remains in source, but its compilation guard excludes it
# from the same image as the authenticated 0x0A owner.
assert "tracker_gateway_batch_release.pending()" in main.split(
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
print("KQ05 single 0x0A owner (0x05/0x09 excluded in combined LAB), LIVE priority: PASS (SOURCE)")
