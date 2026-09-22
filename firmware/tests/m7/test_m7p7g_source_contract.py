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
    'BLE_GATTS_EVT_HVC',
]
for token in required:
    assert token in src, f"missing M7P7G source contract token: {token}"

# WRITE WITHOUT RESPONSE was explicitly excluded by M7P7F.
assert "CHR_PROPS_WRITE_WO_RESP" not in src
# Pinned Bluefruit 1.7.0 BLECharacteristic::indicate() blocks waiting for HVC.
# M7P7G must use the non-blocking SoftDevice HVX submission from loop() instead.
assert "ble_application_response_characteristic.indicate(" not in src

start = src.index("void onBleApplicationWrite(")
end = src.index("\nvoid onBleEvent(", start)
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

print("M7P7G BLE GATT source ownership/property guards: PASS")
