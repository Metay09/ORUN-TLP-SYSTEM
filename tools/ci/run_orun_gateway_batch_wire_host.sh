#!/usr/bin/env bash
# ORUN main-based integration: canonical 0x0A wire + scoped HMAC host tests.
# Host-only; no hardware, custody release, or product runtime activation.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/../.."
tmp="$(mktemp -d "${TMPDIR:-/tmp}/orun-batch-wire.XXXXXXXX")"
trap 'rm -rf -- "$tmp"' EXIT
cxx="${CXX:-g++}"
sanitizers=()
if [[ "${ORUN_HOST_SANITIZERS:-1}" == 1 ]]; then
  sanitizers=(-fsanitize=address,undefined -fno-sanitize-recover=undefined)
fi
"$cxx" -std=gnu++11 -O1 -g -Wall -Wextra -Werror "${sanitizers[@]}" \
  -Ifirmware/include \
  firmware/tests/sf5/test_kq05_gateway_batch_custody_ack_wire.cpp \
  firmware/src/kq05_gateway_batch_custody_ack.cpp \
  -o "$tmp/batch_wire"
"$tmp/batch_wire"
echo "ORUN main-base 0x0A 1..5 wire: PC PASS (PARSER ONLY)"
# Use the EXISTING bounded SHA-256/HMAC crypto API and its host OpenSSL port;
# this is not a second crypto provider or an on-air custody authority.
"$cxx" -std=gnu++11 -O1 -g -Wall -Wextra -Werror "${sanitizers[@]}" \
  -DORUN_KQ05_BATCH_HMAC_HOST_TEST=1 \
  -Ifirmware/include \
  firmware/tests/sf5/test_kq05_gateway_batch_ack_hmac_host.cpp \
  firmware/tests/sf5/sf5k3l_openssl_crypto_test_port.cpp \
  firmware/src/kq05_gateway_batch_custody_crypto.cpp \
  firmware/src/kq05_gateway_batch_custody_ack.cpp \
  firmware/src/tlp_v2_product_secure.cpp \
  -lcrypto -o "$tmp/batch_crypto"
"$tmp/batch_crypto"
echo "ORUN main-base 0x0A 1..5 scoped HMAC: PC PASS (HOST ONLY)"
echo "NO LIVE RF / NO FLASH RELEASE / NO PRODUCT IMAGE CHANGE"

