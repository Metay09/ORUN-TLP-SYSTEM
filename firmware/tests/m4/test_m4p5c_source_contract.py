#!/usr/bin/env python3
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]


def read(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


header = read("firmware/include/history_receipt_receiver.h")
source = read("firmware/src/history_receipt_receiver.cpp")
platformio = read("firmware/platformio.ini")
main = read("firmware/src/main.cpp")

combined = header + "\n" + source

required = [
    "AuthenticatedBackendDurableReceipt authenticated",
    "openBackendDurableReceipt(",
    "submitAuthenticatedReceipt(authenticated)",
    "history_.incarnation()",
    "admission_.pending()",
]
for token in required:
    assert token in combined, f"missing required M4P5C ownership token: {token}"

for forbidden in [
    "currentCredentialId",
    "submitAuthenticatedA2dCounter",
    "markDeliveredThrough",
    "checkpointAcknowledgedDelivery",
    "saveReplayCursor",
    '#include "radio_manager.h"',
    '#include "network_service.h"',
    "sendPositionPacket(",
    "Radio.Send(",
    "Radio.Rx(",
]:
    assert forbidden not in combined, f"forbidden M4P5C coupling: {forbidden}"

# The normal production environment must not enable the target-only probe.
production_env = platformio.split("[env:rak4630]", 1)[1].split(
    "[env:", 1
)[0]
assert "ORUN_M4P5C_HISTORY_RECEIPT_PROBE" not in production_env

assert "[env:rak4630_m4p5c_history_receipt_probe]" in platformio
assert "-D ORUN_M4P5C_HISTORY_RECEIPT_PROBE=1" in platformio

# main.cpp may know only the test-only probe macro; it must not instantiate the
# new production composition seam in normal runtime yet.
assert '#include "history_receipt_receiver.h"' not in main
assert "#ifdef ORUN_M4P5C_HISTORY_RECEIPT_PROBE" in main

print("M4P5C secure History receive composition source-contract guards: PASS")
