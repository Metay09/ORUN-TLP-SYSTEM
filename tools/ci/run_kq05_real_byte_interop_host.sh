#!/usr/bin/env bash
# Cross-language real Backend receipt: Python AEAD + Edge SQLite + signed
# offer -> native SF5L6 BLE fragments -> bounded Gateway HMAC RAM admit.
# Public dummy OGK1 master only. No device, radio or real service mutation.
set -euo pipefail
cd "$(dirname "$0")/../.."

tmp="$(mktemp -d /tmp/orun-kq05-interop.XXXXXX)"
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

PYTHONDONTWRITEBYTECODE=1 python3 \
  firmware/tests/sf5/generate_kq05_real_ble_offer.py "$tmp/real-offer.bin"

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
  firmware/tests/sf5/test_kq05_python_gateway_interop.cpp \
  -lcrypto -o "$tmp/kq05-interop"

"$tmp/kq05-interop" "$tmp/real-offer.bin"
echo "KQ05 verified receipt Python/C++ real-byte interoperability: PASS (HOST ONLY)"
