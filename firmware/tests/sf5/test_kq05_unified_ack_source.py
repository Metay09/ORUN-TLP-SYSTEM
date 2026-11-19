#!/usr/bin/env python3
"""Source-contract only: selected unified 0x0A lab excludes legacy 0x05 owner.
This does not compile RAK firmware or prove RF reception."""
from pathlib import Path

root = Path(__file__).resolve().parents[3]
config = (root / "firmware/platformio.ini").read_text()
main = (root / "firmware/src/main.cpp").read_text()
gw = (root / "firmware/src/kq05_gateway_batch_runtime.cpp").read_text()
head = (root / "firmware/include/kq05_gateway_batch_runtime.h").read_text()
collector = (root / "firmware/src/kq05_gateway_batch_collector.cpp").read_text()
collector_h = (root / "firmware/include/kq05_gateway_batch_collector.h").read_text()
ack = (root / "firmware/include/kq05_gateway_batch_custody_ack.h").read_text()
tracker = (root / "firmware/src/kq05_tracker_batch_radio_runtime.cpp").read_text()
release_owner = (root / "firmware/src/kq05_tracker_batch_release_owner.cpp").read_text()

flag = "ORUN_KQ05_EXPERIMENTAL_GATEWAY_BATCH_RF"
name = "[env:rak4630_kq05_unified_ack_lab]"
assert config.count(name) == 1
before, after = config.split(name)
assert "-D" + flag not in before, "normal images must remain unchanged"
assert "-D" + flag in after.split("[env:", 1)[0]
assert "extends = env:rak4630_kq05_lab" in after.split("[env:", 1)[0]

# Compile-time exclusion of the legacy 0x05 owner and its independent
# credential verifier in the opt-in 0x0A image, NOT default products.
for text in (
    '#ifndef ' + flag + '\n#include "custody_ack_release_owner.h"',
    '#ifndef ' + flag + '\n// Default 0x05 product image',
    '#ifndef ' + flag + '\n  // Legacy-only 0x05 custody release',
    '#ifndef ' + flag + '\n  // SF5K3U: loop-only Tracker ACK intake',
    '#endif // !' + flag,
):
    assert text in main, text
# The new Gateway 0x0A image cannot link a second Backend 0x09
# OBS1 release owner. The original KQ05 LAB still keeps its 0x09 path.
backend_only = '#if defined(ORUN_KQ05_EXPERIMENTAL_ACKLESS_LIVE) && \\\n    !defined(ORUN_KQ05_EXPERIMENTAL_GATEWAY_BATCH_RF)'
assert main.count(backend_only) >= 4
radio_h = (root / "firmware/include/radio_manager.h").read_text()
radio_src = (root / "firmware/src/radio_manager.cpp").read_text()
assert radio_h.count(backend_only) == 4
assert radio_src.count(backend_only) == 4
assert "Kq05BackendReceiptReleaseOwner kq05_batch_release(" in main
assert "tracker_gateway_batch_runtime.receive(" in main
assert "tracker_gateway_batch_release.service()" in main
assert "radio_manager.takeCustodyAckFrame(&ack)" in main  # legacy source kept
assert "bool expedite_events = true" in head
assert "bool trusted_sparse_single_ack = false" in head
assert "kRetryCoalesceMs = 3000U" in head
assert "kDefaultPartialFlushMs = 3000U" in head
# Fail if radio RX window shrinks beneath this nominal software budget.
radio_cfg = (root / "firmware/include/radio_config.h").read_text()
assert "kWindowedRxAfterTxMs = 10000" in radio_cfg
assert "retry_timer_armed_" in gw and "first_retry_ms_" in gw
assert "collector_.queued()==1U" in gw
assert "Receive::kReenrolled" in gw
assert "return Receive::kReenrolled" in collector
assert "kReenrolled" in collector_h
assert "readConfirmedDurable(handle,actual,n)" in collector
assert "kMaxObjects = 5U" in ack
assert "kMaxSize == 88U" in ack
assert "release_.submit(raw.payload,raw.size)" in tracker, "RF adapter must delegate authenticated release"
assert "verifier_.verifyFromStore(frame,length,records_,verified)" in release_owner, "release owner must authenticate full batch before mutation"
assert "records_.requestAuthenticatedCustodyRelease(exact)" in release_owner, "release must use retained exact object"
assert gw.index("radio_.takeProductSecureFrame(&raw)") < gw.index(
    "radio_.sendAuthorizedKq05GatewayBatchAck(collector_)")
print("KQ05 one Gateway ACK 0x0A (count 1..5), old 0x05 compile exclusion: PASS (SOURCE ONLY)")
