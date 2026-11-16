#!/usr/bin/env bash
# SF5F one-hop opaque transport codec, HOST-ONLY. No RF activation.
set -euo pipefail
cd "$(dirname "$0")/../.."
dir="$(mktemp -d "${TMPDIR:-/tmp}/orun-sf5f-relay.XXXXXXXX")"
trap 'rm -rf -- "$dir"' EXIT
cxx="${CXX:-g++}"
flags=(-std=gnu++11 -O1 -g -Wall -Wextra -Werror -Ifirmware/include)
if [[ "${ORUN_HOST_SANITIZERS:-1}" == 1 ]]; then
  flags+=(-fsanitize=address,undefined -fno-sanitize-recover=undefined)
fi
"$cxx" "${flags[@]}" \
  firmware/tests/sf5/test_sf5f_v2_relay_forward.cpp \
  firmware/src/tlp_v2_relay_forward.cpp \
  firmware/src/tlp_v2_product_secure.cpp \
  firmware/src/tlp_v2_delegated_secure_app.cpp \
  firmware/src/custody_ack.cpp \
  -o "$dir/relay"
"$dir/relay"

# The next candidate stays inert: bounded RAM queue, exact-byte duplicate
# admission, explicit due times and ACK-before-telemetry selection only.
"$cxx" "${flags[@]}" \
  firmware/tests/sf5/test_sf5f_v2_relay_queue.cpp \
  firmware/src/v2_relay_forward_queue.cpp \
  firmware/src/tlp_v2_relay_forward.cpp \
  firmware/src/tlp_v2_product_secure.cpp \
  firmware/src/tlp_v2_delegated_secure_app.cpp \
  firmware/src/custody_ack.cpp \
  -o "$dir/queue"
"$dir/queue"

# Actual RadioManager + simulated SX126x owner. Candidate-only switch:
# release rak4630_orun remains unchanged until a separate cutover decision.
PYTHONDONTWRITEBYTECODE=1 python3 firmware/tests/r2/test_patch_radio.py "$dir/driver_bridge.cpp"
radio_flags=(-std=c++17 -O1 -g -Wall -Wextra -Werror
             -Ifirmware/include -Ifirmware/tests/m3/stubs
             -Ifirmware/tests/r2/stubs)
if [[ "${ORUN_HOST_SANITIZERS:-1}" == 1 ]]; then
  radio_flags+=(-fsanitize=address,undefined -fno-sanitize-recover=undefined)
fi
"$cxx" "${radio_flags[@]}" \
  -DORUN_SF5F_V2_RELAY_CANDIDATE -DORUN_SF5K3K_ACK_RX_TEST \
  firmware/tests/sf5/test_sf5f_v2_radio_candidate.cpp \
  "$dir/driver_bridge.cpp" \
  firmware/src/radio_manager.cpp firmware/src/radio_manager_relay_config.cpp \
  firmware/src/v2_relay_forward_queue.cpp firmware/src/tlp_v2_relay_forward.cpp \
  firmware/src/tlp_v2_delegated_secure_app.cpp \
  firmware/src/custody_ack.cpp firmware/src/tlp_v2_product_secure.cpp \
  firmware/src/bridge_frame.cpp firmware/src/radio_listen_policy.cpp \
  firmware/src/network_service.cpp firmware/src/radio_driver_gate.cpp \
  firmware/src/rak_device_identity.cpp firmware/src/legacy_position_mapping.cpp \
  firmware/src/node_role.cpp firmware/src/tlp_test_packet.cpp \
  firmware/src/tlp_position_packet.cpp firmware/src/tlp_relay_forward_packet.cpp \
  firmware/src/tlp_v2_history_secure.cpp \
  -o "$dir/radio"
"$dir/radio"
echo "SF5F V2 codec, RAM queue and radio-owner mock PASS"
echo "NO physical RF, sleepy ACK window, flash or custody HMAC proof"
