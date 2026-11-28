#!/usr/bin/env python3
"""Current product ACK radio source contract."""

from pathlib import Path

repo = Path(__file__).resolve().parents[3]

hdr = (repo / "firmware/include/radio_manager.h").read_text()
cpp = (repo / "firmware/src/radio_manager_custody_ack.cpp").read_text()
runtime = (repo / "firmware/src/gateway_product_custody_runtime.cpp").read_text()

for source in (hdr, cpp, runtime):
    assert "sendAuthorizedCustodyAck(" not in source

assert "sendCustodyAckPacket(" not in hdr

body = cpp.split(
    "RadioManager::BatchTx RadioManager::sendAuthorizedBatchAck(",
    1
)[1]

steps = [
    "if (!canSend()",
    "ingress.localGatewayId() != device_id_",
    "ingress.prepareAuthorizedBatchAck(",
    "radio_driver::Guard gate;",
    "if (!gate || !canSend())",
    "if (!startTxOperation(",
    "tx_kind_ = TxKind::kBatchAck;",
    "Radio.Send(wire,",
    "return BatchTx::kStarted;",
]

last = -1

for needle in steps:
    i = body.find(needle, last + 1)
    assert i > last, needle
    last = i

print("ORUN-305 SINGLE PRIVILEGED ACK RADIO CONTRACT: PASS")
