#!/usr/bin/env bash
# SF5K3K-only actual RadioManager + live SecurityStore scope checks.
# Does NOT repeat SF5K3A-J, does NOT run firmware on real hardware.
set -euo pipefail
cd "$(dirname "$0")/../.."
dir="$(mktemp -d "${TMPDIR:-/tmp}/orun-sf5k3k.XXXXXXXX")"
trap 'rm -rf -- "$dir"' EXIT
cxx="${CXX:-g++}"
flags=(-std=gnu++11 -O1 -g -Wall -Wextra -Werror
       -Wno-conversion -Wno-sign-conversion
       -Ifirmware/include -Ifirmware/tests/sf5)
radio_flags=(-std=c++17 -O1 -g -Wall -Wextra -Werror
             -Ifirmware/include -Ifirmware/tests/m3/stubs
             -Ifirmware/tests/r2/stubs)
if [[ "${ORUN_HOST_SANITIZERS:-1}" == 1 ]]; then
  flags+=(-fsanitize=address,undefined -fno-sanitize-recover=undefined)
  radio_flags+=(-fsanitize=address,undefined -fno-sanitize-recover=undefined)
fi

echo "[SF5K3K] actual SecurityStore persisted Gateway scope + 0x05 wire"
"$cxx" "${flags[@]}" \
  firmware/tests/sf5/test_sf5k3k_custody_ack_scope.cpp \
  firmware/src/security_store.cpp firmware/src/custody_ack.cpp \
  firmware/src/security_v3_page_writer.cpp \
  firmware/src/security_v3_ab_preflight.cpp \
  firmware/src/security_v3_format.cpp \
  firmware/src/tlp_v2_product_secure.cpp \
  firmware/src/security_format.cpp firmware/src/journal_format.cpp \
  firmware/src/tlp_position_packet.cpp \
  -o "$dir/sf5k3k-scope"
"$dir/sf5k3k-scope"

echo "[SF5K3K] actual RadioManager Tracker ACK RX (no HMAC/release)"
PYTHONDONTWRITEBYTECODE=1 python3 firmware/tests/r2/test_patch_radio.py "$dir/driver_bridge.cpp"
"$cxx" "${radio_flags[@]}" -DORUN_SF5K3K_ACK_RX_TEST \
  firmware/tests/sf5/test_sf5j_radio_transport.cpp "$dir/driver_bridge.cpp" \
  firmware/src/radio_manager.cpp firmware/src/custody_ack.cpp \
  firmware/src/radio_manager_relay_config.cpp \
  firmware/src/tlp_v2_product_secure.cpp firmware/src/bridge_frame.cpp \
  firmware/src/radio_listen_policy.cpp firmware/src/network_service.cpp \
  firmware/src/radio_driver_gate.cpp firmware/src/rak_device_identity.cpp \
  firmware/src/legacy_position_mapping.cpp firmware/src/node_role.cpp \
  firmware/src/tlp_test_packet.cpp firmware/src/tlp_position_packet.cpp \
  firmware/src/tlp_relay_forward_packet.cpp firmware/src/tlp_v2_history_secure.cpp \
  -o "$dir/sf5k3k-radio"
"$dir/sf5k3k-radio"
