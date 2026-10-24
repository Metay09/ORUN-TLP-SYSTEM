#!/usr/bin/env bash
# SF5L0 — bench test-key provisioning + Gateway ACK -> real Tracker verifier.
# Host only; independent OpenSSL crypto port. No RF, no device flash.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/../.."
dir="$(mktemp -d "${TMPDIR:-/tmp}/orun-sf5l0.XXXXXXXX")"
trap 'rm -rf -- "$dir"' EXIT
flags=(-std=gnu++11 -O1 -g -Wall -Wextra -Werror
       -Wno-conversion -Wno-sign-conversion
       -DORUN_SF5K3L_HOST_TEST -DORUN_SF5L0_BENCH_TEST_KEYS
       -Ifirmware/include -Ifirmware/tests/sf5)
if [[ "${ORUN_HOST_SANITIZERS:-1}" == 1 ]]; then
  flags+=(-fsanitize=address,undefined -fno-sanitize-recover=undefined)
fi
echo "[SF5L0] bench test-key provisioning + Gateway ACK -> Tracker verifier"
"${CXX:-g++}" "${flags[@]}" \
  firmware/tests/sf5/test_sf5l0_bench_round_trip.cpp \
  firmware/tests/sf5/sf5k3l_openssl_crypto_test_port.cpp \
  firmware/src/sf5l0_bench.cpp firmware/src/edge_link.cpp \
  firmware/src/custody_ack_verifier.cpp firmware/src/custody_ack.cpp \
  firmware/src/security_store.cpp firmware/src/security_v3_page_writer.cpp \
  firmware/src/security_v3_ab_preflight.cpp firmware/src/security_v3_format.cpp \
  firmware/src/security_format.cpp firmware/src/journal_format.cpp \
  firmware/src/tlp_position_packet.cpp firmware/src/tlp_v2_product_secure.cpp \
  firmware/src/gateway_custody_ack_signer.cpp \
  firmware/src/gateway_local_custody_owner.cpp \
  firmware/src/gateway_custody_authorized_ingress.cpp \
  firmware/src/custody_store_product_format.cpp \
  firmware/src/product_custody_store.cpp firmware/src/product_region.cpp \
  -lcrypto -o "$dir/sf5l0"
"$dir/sf5l0"
