#!/usr/bin/env bash
# PC-only real crypto primitive + selective 1..5 Gateway custody ACK MAC.
# No live durable owner, no record release, no radio and no device uploads.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/../.."
dir="$(mktemp -d)"
trap 'rm -rf "$dir"' EXIT

g++ -std=gnu++17 -O1 -g -Wall -Wextra -Wpedantic -Werror \
  -DORUN_KQ05_BATCH_HMAC_HOST_TEST=1 \
  -Ifirmware/include \
  firmware/src/kq05_gateway_batch_custody_ack.cpp \
  firmware/src/kq05_gateway_batch_custody_crypto.cpp \
  firmware/src/custody_ack.cpp \
  firmware/src/tlp_v2_product_secure.cpp \
  firmware/tests/sf5/sf5k3l_openssl_crypto_test_port.cpp \
  firmware/tests/sf5/test_kq05_gateway_batch_ack_hmac_host.cpp \
  -lssl -lcrypto -o "$dir/gateway-batch-hmac"

"$dir/gateway-batch-hmac"
echo "KQ05 GATEWAY 1..5 REAL SHA256/HMAC HOST: PC PASS"
