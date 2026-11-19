#!/usr/bin/env bash
# KQ05 opt-in real SX126x RadioManager type 0x0A RX/TX boundary.
# Executes actual RadioManager host simulated driver and compiles privileged
# Gateway TX source; no RF hardware, Tracker release or firmware upload.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/../.."
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

cxx="${CXX:-g++}"
flags=(-std=c++17 -O1 -g -Wall -Wextra -Werror
  -DORUN_KQ05_EXPERIMENTAL_GATEWAY_BATCH_RF=1
  -Ifirmware/include -Ifirmware/tests/m3/stubs
  -Ifirmware/tests/r2/stubs)

echo "=== REAL RADIOMANAGER 0x0A TRACKER RX / ROLE / SIZE TEST ==="
PYTHONDONTWRITEBYTECODE=1 python3 \
  firmware/tests/r2/test_patch_radio.py "$tmp/driver_bridge.cpp"
"$cxx" "${flags[@]}" \
  firmware/tests/sf5/test_sf5j_radio_transport.cpp \
  "$tmp/driver_bridge.cpp" \
  firmware/src/radio_manager.cpp \
  firmware/src/custody_ack.cpp \
  firmware/src/kq05_gateway_batch_custody_ack.cpp \
  firmware/src/radio_manager_relay_config.cpp \
  firmware/src/tlp_v2_product_secure.cpp \
  firmware/src/bridge_frame.cpp \
  firmware/src/radio_listen_policy.cpp \
  firmware/src/network_service.cpp \
  firmware/src/radio_driver_gate.cpp \
  firmware/src/rak_device_identity.cpp \
  firmware/src/legacy_position_mapping.cpp \
  firmware/src/node_role.cpp \
  firmware/src/tlp_test_packet.cpp \
  firmware/src/tlp_position_packet.cpp \
  firmware/src/tlp_relay_forward_packet.cpp \
  firmware/src/tlp_v2_history_secure.cpp \
  -o "$tmp/real-batch-radio-rx"
"$tmp/real-batch-radio-rx"

echo "=== PRIVILEGED GATEWAY 0x0A SX126x TX C++ COMPILE ==="
"$cxx" "${flags[@]}" -fsyntax-only \
  firmware/src/radio_manager_kq05_gateway_batch_ack.cpp

echo "KQ05 0x0A RADIOMANAGER RX REAL / TX COMPILE: PC PASS"
echo "NO RF OVER THE AIR, NO DEVICE FLASH, NO LEGACY HISTORY ERASE"
