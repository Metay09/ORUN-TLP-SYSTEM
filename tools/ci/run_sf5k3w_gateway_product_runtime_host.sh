#!/usr/bin/env bash
# Real Gateway durable custody/authorization/signing owners, mocked RF calls.
# No real enrollment, on-air RF, role switch, or device flashing.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/../.."
dir="$(mktemp -d "${TMPDIR:-/tmp}/orun-sf5k3w.XXXXXXXX")"
trap 'rm -rf -- "$dir"' EXIT
flags=(-std=gnu++11 -O1 -g -Wall -Wextra -Werror
       -Ifirmware/include -Ifirmware/tests/sf5)
if [[ "${ORUN_HOST_SANITIZERS:-1}" == 1 ]]; then
  flags+=(-fsanitize=address,undefined -fno-sanitize-recover=undefined)
fi
"${CXX:-g++}" "${flags[@]}" \
  firmware/tests/sf5/test_sf5k3w_gateway_product_runtime.cpp \
  firmware/tests/sf5/sf5k3l_openssl_crypto_test_port.cpp \
  firmware/src/gateway_product_custody_runtime.cpp \
  firmware/src/gateway_custody_authorized_ingress.cpp \
  firmware/src/gateway_local_custody_owner.cpp \
  firmware/src/gateway_custody_ack_signer.cpp \
  firmware/src/product_custody_store.cpp \
  firmware/src/custody_store_product_format.cpp \
  firmware/src/product_region.cpp \
  firmware/src/custody_ack.cpp \
  firmware/src/tlp_v2_product_secure.cpp \
  -lcrypto -o "$dir/gateway"
"$dir/gateway"
