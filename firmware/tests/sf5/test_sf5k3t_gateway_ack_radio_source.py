#!/usr/bin/env python3
"""SF5K3T radio TX source-contract regression (not a hardware RF test)."""
from pathlib import Path

repo = Path(__file__).resolve().parents[3]
hdr = (repo / "firmware/include/radio_manager.h").read_text()
legacy_cpp = (repo / "firmware/src/radio_manager.cpp").read_text()
cpp = (repo / "firmware/src/radio_manager_custody_ack.cpp").read_text()
ingress = (repo / "firmware/include/gateway_custody_authorized_ingress.h").read_text()
signature = "bool RadioManager::sendAuthorizedCustodyAck("
assert cpp.count(signature) == 1, "exactly one privileged ACK TX entry point"
assert signature not in legacy_cpp, "privileged ACK cannot impose a Gateway link dependency on legacy radio tests"
assert '#include "gateway_custody_authorized_ingress.h"' not in legacy_cpp
assert '#include "gateway_custody_authorized_ingress.h"' in cpp
body = cpp.split(signature, 1)[1].split("}  // namespace orun_tlp", 1)[0]
assert "bool sendAuthorizedCustodyAck(GatewayCustodyAuthorizedIngress& ingress);" in hdr
assert "uint64_t localGatewayId() const { return gateway_id_; }" in ingress
assert "sendCustodyAckPacket(" not in hdr, "raw ACK TX must not be exposed"
assert "    kCustodyAck," in hdr
assert "  uint32_t custody_ack_tx_attempts = 0;" in hdr
steps = [
    "if (!canSend()",
    "ingress.localGatewayId() != device_id_",
    "ingress.takeAuthorizedAck(ack)",
    "custody_ack::decodeHeader(ack, sizeof(ack), header)",
    "header.gateway_device_id != device_id_",
    "radio_driver::Guard gate;",
    "if (!gate || !canSend())",
    "startTxOperation();",
    "tx_kind_ = TxKind::kCustodyAck;",
    "Radio.Send(ack, static_cast<uint8_t>(sizeof(ack)));",
    "memset(ack, 0, sizeof(ack));",
]
last = -1
for needle in steps:
    i = body.find(needle, last + 1)
    assert i > last, f"missing/out-of-order ACK TX guard: {needle}"
    last = i
assert body.count("memset(ack, 0, sizeof(ack));") >= 3, "wipe on rejection, failed radio admission, TX"
assert "++event_diagnostics_.custody_ack_tx_attempts;" in body
assert "outcome=pending" in body, "TX must not report application delivery"
print("SF5K3T privileged radio ACK source contract: PASS (source only)")
