#!/usr/bin/env bash
# KQ05 real SecurityStore + verified backend HMAC receipt + OBS1 markers.
# Fake NOR only; NO physical flashing or service change.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/../.."
dir="$(mktemp -d "${TMPDIR:-/tmp}/orun-kq05-release.XXXXXXXX")"
trap 'rm -rf -- "$dir"' EXIT
flags=(-std=gnu++11 -O1 -g -Wall -Wextra -Werror
       -Ifirmware/include -Ifirmware/tests/sf5)
if [[ "${ORUN_HOST_SANITIZERS:-1}" == 1 ]]; then
  flags+=(-fsanitize=address,undefined -fno-sanitize-recover=undefined)
fi
"${CXX:-g++}" "${flags[@]}" \
  firmware/tests/sf5/test_kq05_backend_receipt_release.cpp \
  firmware/tests/sf5/sf5k3l_openssl_crypto_test_port.cpp \
  firmware/src/kq05_backend_receipt.cpp \
  firmware/src/kq05_backend_receipt_release_owner.cpp \
  firmware/src/custody_ack_release_owner.cpp \
  firmware/src/custody_ack_product_bridge.cpp \
  firmware/src/custody_ack_verifier.cpp firmware/src/custody_ack.cpp \
  firmware/src/product_record_store.cpp firmware/src/product_region.cpp \
  firmware/src/observation_store.cpp firmware/src/observation_store_control.cpp \
  firmware/src/observation_store_format.cpp \
  firmware/src/product_period_builder.cpp \
  firmware/src/security_store.cpp firmware/src/security_v3_page_writer.cpp \
  firmware/src/security_v3_ab_preflight.cpp firmware/src/security_v3_format.cpp \
  firmware/src/tlp_v2_product_secure.cpp firmware/src/security_format.cpp \
  firmware/src/journal_format.cpp firmware/src/tlp_position_packet.cpp \
  -lcrypto -o "$dir/receipt-release"
"$dir/receipt-release"
