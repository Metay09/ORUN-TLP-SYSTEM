#!/usr/bin/env bash
# KQ05 Gateway bounded five OCQ2 handle batch collector; host NOR only.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/../.."
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

g++ -std=gnu++11 -O1 -g -Wall -Wextra -Werror \
  -Wno-conversion -Wno-sign-conversion \
  -DORUN_KQ05_BATCH_HMAC_HOST_TEST=1 \
  -DORUN_KQ05_EXPERIMENTAL_GATEWAY_BATCH_RF=1 \
  -Ifirmware/include -Ifirmware/tests/sf5 \
  firmware/tests/sf5/test_kq05_gateway_batch_runtime_host.cpp \
  firmware/tests/sf5/sf5k3l_openssl_crypto_test_port.cpp \
  firmware/src/kq05_gateway_batch_runtime.cpp \
  firmware/src/gateway_product_live_ingress.cpp \
  firmware/src/kq05_gateway_batch_collector.cpp \
  firmware/src/kq05_gateway_durable_batch_owner.cpp \
  firmware/src/kq05_gateway_batch_custody_ack.cpp \
  firmware/src/kq05_gateway_batch_custody_crypto.cpp \
  firmware/src/custody_ack.cpp \
  firmware/src/tlp_v2_product_secure.cpp \
  firmware/src/custody_store_product_format.cpp \
  firmware/src/product_custody_store.cpp \
  firmware/src/product_region.cpp \
  -lcrypto -o "$tmp/gateway-batch-scheduler"

"$tmp/gateway-batch-scheduler"
echo "KQ05 GATEWAY 1..5 SCHEDULED DURABLE ACK AND LIVE PRIORITY: PC PASS"
echo "NO REAL RADIO, NO DEVICE FLASH, NO LEGACY HISTORY ERASE"
