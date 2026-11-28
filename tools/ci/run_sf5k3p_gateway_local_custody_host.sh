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
  firmware/src/product_region.cpp \
  firmware/tests/sf5/test_sf5k3p_product_custody_store.cpp \
  -o "$work/product_custody"
"$work/product_custody"
"${CXX:-g++}" "${flags[@]}" -DORUN_GATEWAY_BATCH_ACK=1 \
  firmware/src/custody_store_product_format.cpp \
  firmware/src/product_custody_store.cpp \
  firmware/src/product_region.cpp \
  firmware/src/gateway_local_custody_owner.cpp \
  firmware/src/gateway_local_custody_owner_batch.cpp \
  firmware/src/gateway_custody_authorized_ingress.cpp \
  firmware/src/gateway_custody_authorized_batch.cpp \
  firmware/src/kq05_gateway_batch_custody_crypto.cpp \
  firmware/src/kq05_gateway_batch_custody_ack.cpp \
  firmware/src/tlp_v2_product_secure.cpp \
  firmware/tests/sf5/sf5k3l_openssl_crypto_test_port.cpp \
  firmware/tests/sf5/test_sf5k3p_gateway_local_owner.cpp \
  -lcrypto -o "$work/gateway_owner"
"$work/gateway_owner"
