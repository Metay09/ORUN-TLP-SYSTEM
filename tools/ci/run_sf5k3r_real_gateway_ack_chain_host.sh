#!/usr/bin/env bash
# Compiles ACTUAL production GatewayLocalCustodyOwner, ACK signer, store,
# product codec + ACK parser, with test-only independent OpenSSL crypto.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/../.."
dir="$(mktemp -d "${TMPDIR:-/tmp}/orun-sf5k3r.XXXXXXXX")"
trap 'rm -rf -- "$dir"' EXIT
flags=(-std=gnu++11 -O1 -g -Wall -Wextra -Werror -Ifirmware/include)
if [[ "${ORUN_HOST_SANITIZERS:-1}" == 1 ]]; then
  flags+=(-fsanitize=address,undefined -fno-sanitize-recover=undefined)
fi
"${CXX:-g++}" "${flags[@]}" \
  firmware/tests/sf5/test_sf5k3r_real_gateway_ack_chain.cpp \
  firmware/tests/sf5/sf5k3l_openssl_crypto_test_port.cpp \
  firmware/src/gateway_custody_ack_signer.cpp \
  firmware/src/gateway_local_custody_owner.cpp \
  firmware/src/custody_store_product_format.cpp \
  firmware/src/product_custody_store.cpp \
  firmware/src/product_region.cpp \
  firmware/src/custody_ack.cpp \
  firmware/src/tlp_v2_product_secure.cpp \
  -lcrypto -o "$dir/real_gateway_ack"
"$dir/real_gateway_ack"
