#!/usr/bin/env bash
# Y2a — the Tracker/Gateway runtime built like the product images: without
# ORUN_SF5L0_BENCH_TEST_KEYS. Host only; independent OpenSSL crypto port.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/../.."
dir="$(mktemp -d "${TMPDIR:-/tmp}/orun-y2a.XXXXXXXX")"
trap 'rm -rf -- "$dir"' EXIT
flags=(-std=gnu++11 -O1 -g -Wall -Wextra -Werror
       -Wno-conversion -Wno-sign-conversion
       -DORUN_SF5K3L_HOST_TEST -DORUN_SF5L0_TRACKER
       -Ifirmware/include -Ifirmware/tests/sf5)
if [[ "${ORUN_HOST_SANITIZERS:-1}" == 1 ]]; then
  flags+=(-fsanitize=address,undefined -fno-sanitize-recover=undefined)
fi
echo "[Y2a] product build: no public test credential"
# The Tracker product build must not carry the public test root or the
# test-credential provisioning entry point.
"${CXX:-g++}" "${flags[@]}" -c firmware/src/sf5l0_bench.cpp -o "$dir/bench.o"
if nm -C "$dir/bench.o" | grep -E "kTestRoot|Sf5l0BenchTestProvisioning::start\(unsigned long" ; then
  echo "FAIL: test root or test-key provisioning compiled into the product build" >&2
  exit 1
fi
# The Gateway product build differs only in keeping the test root for the
# Edge accept MAC until Y2b (device headers keep the real Gateway build off
# the host): compile that combination too, still without the test-key paths.
"${CXX:-g++}" "${flags[@]}" -DORUN_SF5L0_HAS_TEST_ROOT=1 -c firmware/src/sf5l0_bench.cpp -o "$dir/bench_gw.o"
if nm -C "$dir/bench_gw.o" | grep -E "Sf5l0BenchTestProvisioning::start\(unsigned long" ; then
  echo "FAIL: test-key provisioning compiled into the Gateway product build" >&2
  exit 1
fi
"${CXX:-g++}" "${flags[@]}" \
  firmware/tests/sf5/test_y2a_product_keys.cpp \
  firmware/tests/sf5/sf5k3l_openssl_crypto_test_port.cpp \
  "$dir/bench.o" firmware/src/edge_link.cpp \
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
  -lcrypto -o "$dir/y2a"
"$dir/y2a"
