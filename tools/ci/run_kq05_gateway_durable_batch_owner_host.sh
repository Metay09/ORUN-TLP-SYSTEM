#!/usr/bin/env bash
# KQ05 actual OCQ2 committed-readback five-object Gateway batch issuer.
# PC/NOR simulation only; no RF, no device upload, no History erase.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/../.."
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

g++ -std=gnu++17 -O1 -g -Wall -Wextra -Werror \
  -DORUN_KQ05_BATCH_HMAC_HOST_TEST=1 \
  -Ifirmware/include \
  firmware/src/kq05_gateway_durable_batch_owner.cpp \
  firmware/src/kq05_gateway_batch_custody_ack.cpp \
  firmware/src/kq05_gateway_batch_custody_crypto.cpp \
  firmware/src/custody_ack.cpp \
  firmware/src/tlp_v2_product_secure.cpp \
  firmware/src/custody_store_product_format.cpp \
  firmware/src/product_custody_store.cpp \
  firmware/src/product_region.cpp \
  firmware/tests/sf5/sf5k3l_openssl_crypto_test_port.cpp \
  firmware/tests/sf5/test_kq05_gateway_durable_batch_owner_host.cpp \
  -lssl -lcrypto -o "$tmp/kq05-gateway-durable-batch"

"$tmp/kq05-gateway-durable-batch"
echo "KQ05 GATEWAY FIVE-RECORD FLASH DURABILITY -> SCOPED HMAC: PC PASS"
