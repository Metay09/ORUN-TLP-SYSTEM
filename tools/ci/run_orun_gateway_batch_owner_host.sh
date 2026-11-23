#!/usr/bin/env bash
# Main-based OCQ2 Gateway local owner: 0x0A batch HMAC and old 0x05 regression.
# Host only. Deliberately no GitHub Actions, board upload or flash mutation.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/../.."
tmp="$(mktemp -d "${TMPDIR:-/tmp}/orun-main-batch-owner.XXXXXXXX")"
trap 'rm -rf -- "$tmp"' EXIT
flags=(-std=gnu++11 -O1 -g -Wall -Wextra -Werror
       -DORUN_KQ05_BATCH_HMAC_HOST_TEST=1 -Ifirmware/include)
if [[ "${ORUN_HOST_SANITIZERS:-1}" == 1 ]]; then
  flags+=(-fsanitize=address,undefined -fno-sanitize-recover=undefined)
fi
sources=(
  firmware/tests/sf5/sf5k3l_openssl_crypto_test_port.cpp
  firmware/src/gateway_custody_authorized_ingress.cpp
  firmware/src/gateway_custody_authorized_batch.cpp
  firmware/src/gateway_local_custody_owner.cpp
  firmware/src/gateway_local_custody_owner_batch.cpp
  firmware/src/gateway_custody_ack_signer.cpp
  firmware/src/kq05_gateway_batch_custody_crypto.cpp
  firmware/src/kq05_gateway_batch_custody_ack.cpp
  firmware/src/custody_store_product_format.cpp
  firmware/src/product_custody_store.cpp
  firmware/src/product_region.cpp
  firmware/src/custody_ack.cpp
  firmware/src/tlp_v2_product_secure.cpp
)
"${CXX:-g++}" "${flags[@]}" \
  firmware/tests/sf5/test_orun_main_gateway_batch_owner.cpp "${sources[@]}" \
  -lcrypto -o "$tmp/batch_owner"
"$tmp/batch_owner"

python3 firmware/tests/r2/test_patch_radio.py "$tmp/driver_bridge.cpp"
"${CXX:-g++}" "${flags[@]}" -std=c++17 -DORUN_GATEWAY_BATCH_ACK=1 \
  -Ifirmware/tests/r2/stubs -Ifirmware/tests/m3/stubs \
  firmware/tests/sf5/test_gateway_ack_radio_window.cpp "$tmp/driver_bridge.cpp" \
  firmware/src/radio_manager.cpp firmware/src/radio_manager_custody_ack.cpp \
  firmware/src/radio_manager_relay_config.cpp firmware/src/lora_command_frame.cpp \
  firmware/src/radio_driver_gate.cpp firmware/src/rak_device_identity.cpp \
  firmware/src/radio_listen_policy.cpp firmware/src/network_service.cpp \
  firmware/src/bridge_frame.cpp firmware/src/legacy_position_mapping.cpp \
  firmware/src/node_role.cpp firmware/src/tlp_test_packet.cpp \
  firmware/src/tlp_position_packet.cpp firmware/src/tlp_relay_forward_packet.cpp \
  firmware/src/tlp_v2_history_secure.cpp "${sources[@]}" -lcrypto -o "$tmp/ack_window"
"$tmp/ack_window"
