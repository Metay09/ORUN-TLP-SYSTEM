#!/usr/bin/env bash
# SF5K3P: no RF/credentials, real portable OCQ2-v3 Gateway storage gate,
# fake NOR fault/reboot/async tests and a test-only private-signer stand-in.
# The real SF5K3N HMAC vectors are checked by their separate focused suite.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/../.."
work="$(mktemp -d "${TMPDIR:-/tmp}/orun-sf5k3p.XXXXXX")"
trap 'rm -rf -- "$work"' EXIT
flags=(-std=gnu++11 -O1 -g -Wall -Wextra -Werror -Ifirmware/include)
if [[ "${ORUN_HOST_SANITIZERS:-1}" == 1 ]]; then
  flags+=(-fsanitize=address,undefined -fno-sanitize-recover=undefined)
fi
"${CXX:-g++}" "${flags[@]}" \
  firmware/src/custody_store_product_format.cpp \
  firmware/src/product_custody_store.cpp \
  firmware/tests/sf5/test_sf5k3p_product_custody_store.cpp \
  -o "$work/product_custody"
"$work/product_custody"
"${CXX:-g++}" "${flags[@]}" \
  firmware/src/custody_store_product_format.cpp \
  firmware/src/product_custody_store.cpp \
  firmware/src/gateway_local_custody_owner.cpp \
  firmware/tests/sf5/test_sf5k3p_gateway_local_owner.cpp \
  -o "$work/gateway_owner"
"$work/gateway_owner"
