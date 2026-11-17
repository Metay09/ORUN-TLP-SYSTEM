#!/usr/bin/env python3
"""KQ05 LAB only: fail source checks if dual legacy/product writers reappear.

This is a source wiring invariant, NOT a substitute for target build, host
integration or real hardware evidence. The legacy production path stays intact.
"""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
code = (ROOT / "firmware/src/main.cpp").read_text(encoding="utf-8")
ini = (ROOT / "firmware/platformio.ini").read_text(encoding="utf-8")


def must(text: str) -> None:
    assert code.count(text) == 1, f"KQ05 one-source invariant missing/ambiguous: {text!r}"


assert "[env:rak4630_kq05_lab]" in ini
assert "-DORUN_KQ05_EXPERIMENTAL_ACKLESS_LIVE" in ini
must("bool kq05ProductOnlyTracker()")
must("return trackerStorage();")
must("tracking_enabled && !kq05ProductOnlyTracker() &&")
must("? product_write_ready : positions.prepareForFixStorage()")
must("const bool accepted_product = finishProductPeriod(&fix);")
must("const bool accepted = kq05ProductOnlyTracker()")
must(": positions.acceptFix(fix, orun_tlp::monotonic::nowMs());")
must("if (ble_ready && !kq05ProductOnlyTracker()) {")
must("if (!kq05ProductOnlyTracker()) history.poll();")
must('Serial.println(F("KQ05 STORAGE owner=OBS1; legacy HistoryStore NOT MOUNTED"));')
assert code.count("history.begin(device_identity.legacyUint64())") == 2
assert "#ifndef ORUN_KQ05_EXPERIMENTAL_ACKLESS_LIVE" in code
must('Serial.println(F("KQ05 GEOFENCE representative suppressed; EVENT owns occurrence"));')
must("queueKq05OutsideEvent(fix);")
must("product_records.submitEvent(kq05_event_queue[kq05_event_head])")
must("if (kq05_event_count > 0U) {")
assert code.count("kq05_event_awaiting_durable = false;") == 2  # boot + durable-outcome completion
assert "KQ05 GEOFENCE representative PRODUCT EVENT not implemented" not in code

must('Serial.println(F("KQ05 PRODUCT only: PERIODIC not accepted; no legacy fallback"));')
must("!product_period_open && product_records.mounted() &&")
must("openProductPeriod(orun_tlp::monotonic::nowMs());")

# All remaining legacy POSITION mutations must be behind the non-KQ05 branch.
assert code.count("positions.acceptFix(") == 1
assert code.count("positions.acceptPreviouslyAcceptedFix(") == 1
assert code.count("history_store_forward.update(") == 1
# The product period writer must still exist once for all builds.
assert code.count("product_records.submitPeriodic(in)") == 1
# No history erase/discard was added to the migration.
assert "kq05ProductOnlyTracker() &&" in code

print("KQ05 single PERIODIC owner; legacy POSITION/HISTORY suppressed only in LAB Tracker: PASS (SOURCE ONLY)")
