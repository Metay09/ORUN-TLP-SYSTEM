#!/usr/bin/env bash
# KQ05 isolated, real native Gateway RAM relay HMAC and TX gating; no USB,
# actual RF, device flashing, production credentials, or Gateway Flash mutation.
set -euo pipefail
cd "$(dirname "$0")/../.."

tmp="$(mktemp -d /tmp/orun-kq05-ramrelay.XXXXXX)"
cleanup() {
  local status=$?
  if [[ -n "$tmp" && -d "$tmp" && ! -L "$tmp" ]]; then
    rm -rf -- "$tmp"
  fi
  return "$status"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

flags=(-std=c++17 -O1 -g -Wall -Wextra -Werror
       -DORUN_KQ05_EXPERIMENTAL_ACKLESS_LIVE
       -Ifirmware/include)
if [[ "${ORUN_HOST_SANITIZERS:-1}" == "1" ]]; then
  flags+=(-fsanitize=address,undefined -fno-sanitize-recover=undefined)
fi

"${CXX:-g++}" "${flags[@]}" \
  firmware/src/kq05_gateway_ram_relay.cpp \
  firmware/src/kq05_edge_link_wire.cpp \
  firmware/src/edge_link.cpp \
  firmware/tests/sf5/sf5k3l_openssl_crypto_test_port.cpp \
  firmware/tests/sf5/test_kq05_gateway_ram_relay.cpp \
  -lcrypto -o "$tmp/kq05_ramrelay"

"$tmp/kq05_ramrelay"
echo "KQ05 Gateway native RAM relay HMAC/scope/priority/expiry: PASS (HOST ONLY)"
