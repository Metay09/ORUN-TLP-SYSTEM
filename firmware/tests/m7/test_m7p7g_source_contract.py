#!/usr/bin/env python3
from pathlib import Path

src = Path("firmware/src/main.cpp").read_text()

required = [
    '#include "ble_application_handoff.h"',
    '#include "ble_application_transport.h"',
    'ble_application_request_characteristic.setProperties(CHR_PROPS_WRITE);',
    'ble_application_response_characteristic.setProperties(CHR_PROPS_INDICATE);',
    'ble_application_request_characteristic.setWriteCallback(',
    'onBleApplicationWrite, false);',
    'sd_ble_gatts_hvx',
    'NRF_ERROR_TIMEOUT',
    'BleApplicationIndicationSubmitResult::kTerminal',
    'BLE_GATTS_EVT_HVC',
    'BLE_GATTS_EVT_TIMEOUT',
    'BLE_GATT_TIMEOUT_SRC_PROTOCOL',
    'enqueueGattTimeout',
    'Bluefruit.disconnect',
    'evt->evt.gatts_evt.conn_handle',
]
for token in required:
    assert token in src, f"missing M7P7G source contract token: {token}"

# WRITE WITHOUT RESPONSE was explicitly excluded by M7P7F.
assert "CHR_PROPS_WRITE_WO_RESP" not in src
# Pinned Bluefruit 1.7.0 BLECharacteristic::indicate() blocks waiting for HVC.
# M7P7G must use the non-blocking SoftDevice HVX submission from loop() instead.
assert "ble_application_response_characteristic.indicate(" not in src

start = src.index("void onBleApplicationWrite(")
brace = src.index("{", start)
depth = 0
end = None
for index in range(brace, len(src)):
    if src[index] == "{":
        depth += 1
    elif src[index] == "}":
        depth -= 1
        if depth == 0:
            end = index + 1
            break
assert end is not None, "unterminated onBleApplicationWrite() body"
callback = src[start:end]
for forbidden in (
    "Serial",
    "Bluefruit",
    "application_requests",
    "config_store",
    "radio_manager",
    "sd_ble_",
    "monotonic::",
):
    assert forbidden not in callback, (
        f"BLE write callback must stay bounded handoff-only; found {forbidden}"
    )

event_start = src.index("void onBleEvent(")
event_brace = src.index("{", event_start)
depth = 0
event_end = None
for index in range(event_brace, len(src)):
    if src[index] == "{":
        depth += 1
    elif src[index] == "}":
        depth -= 1
        if depth == 0:
            event_end = index + 1
            break
assert event_end is not None, "unterminated onBleEvent() body"
event_callback = src[event_start:event_end]
for forbidden in (
    "Bluefruit.",
    "Serial",
    "sd_ble_gap_disconnect",
    "application_requests",
    "config_store",
    "radio_manager",
    "monotonic::",
):
    assert forbidden not in event_callback, (
        f"BLE event callback must stay bounded handoff-only; found {forbidden}"
    )

runtime_start = src.index("void pollBleApplicationRuntime(")
runtime_brace = src.index("{", runtime_start)
depth = 0
runtime_end = None
for index in range(runtime_brace, len(src)):
    if src[index] == "{":
        depth += 1
    elif src[index] == "}":
        depth -= 1
        if depth == 0:
            runtime_end = index + 1
            break
assert runtime_end is not None, "unterminated pollBleApplicationRuntime() body"
runtime = src[runtime_start:runtime_end]
assert "takeGattTimeout" in runtime
assert "endBleApplicationSession();" in runtime
assert "BLE indication submit terminal error=" in runtime
assert "BleApplicationIndicationSubmitResult::kRetryable" in runtime
assert "BleApplicationIndicationSubmitResult::kTerminal" in runtime

recovery_start = src.index("bool serviceBleApplicationDisconnectRecovery(")
recovery_brace = src.index("{", recovery_start)
depth = 0
recovery_end = None
for index in range(recovery_brace, len(src)):
    if src[index] == "{":
        depth += 1
    elif src[index] == "}":
        depth -= 1
        if depth == 0:
            recovery_end = index + 1
            break
assert recovery_end is not None, "unterminated disconnect recovery body"
recovery = src[recovery_start:recovery_end]
assert "Bluefruit.disconnect" in recovery

print("M7P7G BLE GATT source ownership/property/timeout guards: PASS")
