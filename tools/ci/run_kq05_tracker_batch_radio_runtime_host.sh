#!/usr/bin/env bash
# KQ05 0x0A Gateway custody group -> real Tracker V3 HMAC + five
# separately committed OBS1 release markers, HOST simulated NOR ONLY.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/../.."
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

echo "=== KQ05 MAIN LOOP / 0x05 / 0x09 / LIVE PRIORITY SOURCE CONTRACT ==="
PYTHONDONTWRITEBYTECODE=1 python3 \
  firmware/tests/sf5/test_kq05_tracker_batch_radio_runtime_source.py

g++ -std=gnu++11 -O1 -g -Wall -Wextra -Werror \
  -Wno-conversion -Wno-sign-conversion \
  -DORUN_KQ05_BATCH_HMAC_HOST_TEST=1 \
  -DORUN_KQ05_TRACKER_BATCH_VERIFY_HOST_TEST=1 \
  -DORUN_KQ05_EXPERIMENTAL_GATEWAY_BATCH_RF=1 \
  -Ifirmware/include -Ifirmware/tests/sf5 \
  firmware/tests/sf5/test_kq05_tracker_batch_radio_runtime_host.cpp \
  firmware/tests/sf5/sf5k3l_openssl_crypto_test_port.cpp \
  firmware/src/kq05_tracker_batch_radio_runtime.cpp \
  firmware/src/kq05_tracker_batch_release_owner.cpp \
  firmware/src/kq05_tracker_batch_ack_verifier.cpp \
  firmware/src/kq05_gateway_batch_custody_crypto.cpp \
  firmware/src/kq05_gateway_batch_custody_ack.cpp \
  firmware/src/custody_ack_release_owner.cpp \
  firmware/src/custody_ack_product_bridge.cpp \
  firmware/src/custody_ack_verifier.cpp firmware/src/custody_ack.cpp \
  firmware/src/product_record_store.cpp firmware/src/product_region.cpp \
  firmware/src/observation_store.cpp \
  firmware/src/observation_store_control.cpp \
  firmware/src/observation_store_format.cpp \
  firmware/src/product_period_builder.cpp \
  firmware/src/security_store.cpp \
  firmware/src/security_v3_page_writer.cpp \
  firmware/src/security_v3_ab_preflight.cpp \
  firmware/src/security_v3_format.cpp \
  firmware/src/tlp_v2_product_secure.cpp \
  firmware/src/security_format.cpp \
  firmware/src/journal_format.cpp \
  firmware/src/tlp_position_packet.cpp \
  -lcrypto -o "$tmp/tracker-batch-radio-runtime"

"$tmp/tracker-batch-selective-release"
echo "KQ05 TRACKER RADIO 0x0A -> REAL V3 HMAC -> SELECTIVE OBS1 RELEASE: PC PASS"
echo "NO PHYSICAL RADIO, NO REAL NOR, NO LEGACY HISTORY ERASE, NO DEVICE UPLOAD"
