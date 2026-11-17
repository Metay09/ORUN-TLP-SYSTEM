#!/usr/bin/env bash
# KQ05 isolated signed OGK1 v2 Edge link/Tracker epoch admission.
# Fake issuer proof is HOST-ONLY; no provisioning, RF, Flash write or device.
set -euo pipefail
cd "$(dirname "$0")/../.."

tmp="$(mktemp -d /tmp/orun-kq05-ogk1.XXXXXX)"
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
       -DORUN_KQ05_EXPERIMENTAL_ACKLESS_LIVE -Ifirmware/include)
if [[ "${ORUN_HOST_SANITIZERS:-1}" == "1" ]]; then
  flags+=(-fsanitize=address,undefined -fno-sanitize-recover=undefined)
fi
"${CXX:-g++}" "${flags[@]}" \
  firmware/src/gateway_grant_snapshot_reader.cpp \
  firmware/src/observation_store_format.cpp \
  firmware/src/kq05_gateway_signed_link_credentials.cpp \
  firmware/src/kq05_gateway_ram_relay.cpp \
  firmware/src/kq05_edge_link_wire.cpp \
  firmware/src/edge_link.cpp \
  firmware/tests/sf5/sf5k3l_openssl_crypto_test_port.cpp \
  firmware/tests/sf5/test_kq05_gateway_signed_link_credentials.cpp \
  -lcrypto -o "$tmp/kq05_ogk1"

"$tmp/kq05_ogk1"
echo "KQ05 signed OGK1 v2 dedicated link/epoch admission: PASS (FAKE ISSUER HOST ONLY)"
