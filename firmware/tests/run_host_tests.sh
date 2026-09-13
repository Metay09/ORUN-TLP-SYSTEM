#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."
test_dir=$(mktemp -d /tmp/orun-host-tests.XXXXXX)
flags=(-std=c++17 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined
       -Ifirmware/tests/m3/stubs -Ifirmware/include)
g++ "${flags[@]}" firmware/tests/m3/test_m3.cpp firmware/src/gnss_manager.cpp \
  firmware/src/gnss_utc.cpp -o "$test_dir/m3"
"$test_dir/m3"
g++ "${flags[@]}" firmware/tests/r3/test_delayed_pair.cpp firmware/src/gnss_manager.cpp \
  firmware/src/gnss_utc.cpp -o "$test_dir/r3_delayed"
"$test_dir/r3_delayed"
g++ "${flags[@]}" firmware/tests/r3/test_r3.cpp firmware/src/gnss_manager.cpp \
  firmware/src/gnss_utc.cpp firmware/src/tlp_position_packet.cpp \
  firmware/src/node_role.cpp -o "$test_dir/r3"
"$test_dir/r3"
g++ "${flags[@]}" firmware/tests/m4/test_m4.cpp firmware/src/history_store.cpp \
  firmware/src/journal_format.cpp firmware/src/position_flow.cpp \
  firmware/src/tlp_position_packet.cpp -o "$test_dir/m4"
"$test_dir/m4"
g++ -Ifirmware/tests/m4/nrf_stubs "${flags[@]}" -fno-pie -no-pie \
  -Wl,--defsym,__flash_arduino_end=0xED000 \
  firmware/tests/m4/test_nrf_backend.cpp firmware/src/nrf_history_flash.cpp \
  firmware/src/history_store.cpp firmware/src/journal_format.cpp \
  firmware/src/position_flow.cpp firmware/src/tlp_position_packet.cpp \
  -o "$test_dir/nrf_backend"
"$test_dir/nrf_backend"
g++ "${flags[@]}" firmware/tests/m5/test_m5.cpp \
  firmware/src/network_service.cpp firmware/src/node_role.cpp \
  firmware/src/tlp_position_packet.cpp firmware/src/tlp_relay_forward_packet.cpp \
  -o "$test_dir/m5"
"$test_dir/m5"
PYTHONDONTWRITEBYTECODE=1 python3 firmware/tests/r2/test_patch_radio.py "$test_dir/driver_bridge.cpp"
g++ -Ifirmware/tests/r2/stubs "${flags[@]}" firmware/tests/r2/test_r2.cpp \
  "$test_dir/driver_bridge.cpp" \
  firmware/src/radio_manager.cpp firmware/src/network_service.cpp \
  firmware/src/radio_driver_gate.cpp \
  firmware/src/node_role.cpp firmware/src/tlp_test_packet.cpp \
  firmware/src/tlp_position_packet.cpp firmware/src/tlp_relay_forward_packet.cpp \
  -o "$test_dir/r2"
"$test_dir/r2"
