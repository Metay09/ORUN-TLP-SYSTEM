#!/usr/bin/env python3
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]


def read(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


receiver_h = read("firmware/include/history_receipt_receiver.h")
receiver_cpp = read("firmware/src/history_receipt_receiver.cpp")
radio_h = read("firmware/include/radio_manager.h")
radio_cpp = read("firmware/src/radio_manager.cpp")
platformio = read("firmware/platformio.ini")
main = read("firmware/src/main.cpp")

receiver = receiver_h + "\n" + receiver_cpp
radio = radio_h + "\n" + radio_cpp

for token in [
    "AuthenticatedBackendDurableReceipt authenticated",
    "openBackendDurableReceipt(",
    "submitAuthenticatedReceipt(authenticated)",
    "history_.incarnation()",
    "admission_.pending()",
]:
    assert token in receiver, f"missing secure receipt ownership token: {token}"

for forbidden in [
    "currentCredentialId",
    "submitAuthenticatedA2dCounter",
    "markDeliveredThrough",
    "checkpointAcknowledgedDelivery",
    "saveReplayCursor",
    '#include "radio_manager.h"',
    '#include "network_service.h"',
]:
    assert forbidden not in receiver, f"receipt owner leaked responsibility: {forbidden}"

# Normal production is now intentionally wired: no more host-only composition.
for token in [
    '#include "history_receipt_receiver.h"',
    "HistorySecureCrypto history_secure_crypto(security_store)",
    "HistoryDeliveryCoordinator history_delivery(history)",
    "HistoryReceiptAdmissionCoordinator history_receipt_admission(",
    "HistoryReceiptReceiver history_receipt_receiver(",
    "serviceHistoryReceiptAdmission();",
    "ingestHistorySecureRadioFrame();",
    "serviceHistoryReplay(orun_tlp::monotonic::nowMs());",
    "history.readNextRetained(history.acknowledgedThrough(), record)",
    "STORE-FORWARD runtime=ACTIVE",
]:
    assert token in main, f"normal production store-forward wiring missing: {token}"

# Runtime must remain replay-safe/wear-safe in this activation slice.
for forbidden in [
    "history.markDeliveredThrough(",
    "history.checkpointAcknowledgedDelivery(",
    "history.saveReplayCursor(",
    "history.getOldestUndelivered(record)",
]:
    assert forbidden not in main, f"forbidden initial runtime behavior: {forbidden}"

# Radio performs only bounded raw-byte handoff and exact protected-observation
# transport. Crypto/application delivery stay outside the driver gate.
for token in [
    "sendHistorySecurePacket(",
    "takeHistorySecureRx(",
    "history_secure_rx_pending_",
    "kHistorySecureMaxPacketSize",
    "kHistorySecurityContextDeviceD2a",
]:
    assert token in radio, f"History secure radio runtime seam missing: {token}"

for forbidden in [
    "HistorySecureCrypto",
    "HistoryReceiptAdmissionCoordinator",
    "submitAuthenticatedA2dCounter",
]:
    assert forbidden not in radio, f"crypto/security owner leaked into RadioManager: {forbidden}"

# The target KAT is still test-only; the normal production env does not define
# its macro. Production activation comes from ordinary source composition.
production_env = platformio.split("[env:rak4630]", 1)[1].split(
    "[env:", 1
)[0]
assert "ORUN_M4P5C_HISTORY_RECEIPT_PROBE" not in production_env
assert "[env:rak4630_m4p5c_history_receipt_probe]" in platformio

print("M4P5C active device store-forward source-contract guards: PASS")
