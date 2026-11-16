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
echo "SF5F v2 opaque relay codec + RAM queue PASS (host only; no RF/ACK proof)"
