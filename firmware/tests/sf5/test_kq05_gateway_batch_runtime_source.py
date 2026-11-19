#!/usr/bin/env python3
"""KQ05 opt-in Gateway composition source gates; not device RF verification."""
from pathlib import Path

root = Path(__file__).resolve().parents[3]
gateway = (root / "firmware/src/sf5l0_bench_gateway.cpp").read_text()
runtime = (root / "firmware/src/kq05_gateway_batch_runtime.cpp").read_text()
collector = (root / "firmware/src/kq05_gateway_batch_collector.cpp").read_text()
handoff = (root / "firmware/src/sf5l0_bench.cpp").read_text()
platform = (root / "firmware/platformio.ini").read_text()
radio = (root / "firmware/src/radio_manager_kq05_gateway_batch_ack.cpp").read_text()

flag = "ORUN_KQ05_EXPERIMENTAL_GATEWAY_BATCH_RF"
assert "-D" + flag not in platform.split("[env:rak4630_kq05_unified_ack_lab]",1)[0], "regular images must not enable batch"
assert "-D" + flag in platform.split("[env:rak4630_kq05_unified_ack_lab]",1)[1].split("[env:",1)[0], "dedicated lab target must opt in"
for needle in (
    '#include "kq05_gateway_batch_collector.h"',
    '#include "kq05_gateway_batch_runtime.h"',
    "collector = new Kq05GatewayBatchCollector(*custody, *grants, gateway_id);",
    "batch_runtime = new Kq05GatewayBatchRuntime(",
    "batch_runtime->updateGuarded(",
    "guard->inspectAndMount(true, observation_store_mounted, gateway_id)",
    "handoff->service(collector == nullptr || collector->queued()==0U)",
    "live_ingress = new GatewayProductLiveIngress(*custody, *grants, gateway_id);",
    "ORUN_KQ05_EXPERIMENTAL_ACKLESS_LIVE",
):
    assert needle in gateway, needle
assert gateway.count("#ifdef " + flag) >= 5
assert "runtime = new GatewayProductCustodyRuntime" in gateway
assert "new GatewayCustodyAuthorizedIngress" in gateway
assert "serviceHandoff();" in gateway
assert "allow_new_reclaim && check_reclaim_" in handoff
assert "bool allow_new_reclaim = true" in (
    root / "firmware/include/sf5l0_bench.h"
).read_text()
assert "collector_.pendingFlash()" in runtime
assert "live_.pending()" in runtime
assert "collector_.service()" in runtime
assert "live_.service()" in runtime
assert "now_ms-first_durable_ms_" in runtime
assert "collector_.requestFlush()" in runtime
assert "radio_.takeProductSecureFrame(&raw)" in runtime
assert "live_.receive(raw.payload,raw.size)" in runtime
assert "collector_.receive(raw.payload,raw.size)" in runtime
assert "radio_.sendAuthorizedKq05GatewayBatchAck(collector_)" in runtime
assert runtime.index("radio_.takeProductSecureFrame(&raw)") < runtime.index(
    "radio_.sendAuthorizedKq05GatewayBatchAck(collector_)"
), "new LIVE RX must take precedence over pending batch radio TX"
assert "signer_.issue(" in collector
assert "readConfirmedDurable(handle,actual,n)" in collector
assert "collector.markTxAdmitted()" in radio
assert radio.index("Radio.Send(bytes,static_cast<uint8_t>(length));") < radio.index(
    "collector.markTxAdmitted()"
)
print("KQ05 Gateway loop 1..5 timed ACK / LIVE priority / Edge reclaim isolation: PASS (source contract)")
