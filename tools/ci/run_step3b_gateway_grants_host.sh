#!/usr/bin/env bash
# Step 3B — backend-signed OGK1 grant snapshot installed over USB.
# Host only: the Python backend builds/signs the snapshots, the C++ test
# replays the exact USB lines through the firmware installer and boot reader
# (OpenSSL Ed25519 in place of the pinned CC310 verifier, fake NOR).
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/../.."
dir="$(mktemp -d "${TMPDIR:-/tmp}/orun-step3b.XXXXXXXX")"
trap 'rm -rf -- "$dir"' EXIT
PYTHONDONTWRITEBYTECODE=1 python3 firmware/tests/sf5/test_step3b_gateway_grants.py \
  --vectors "$dir/vectors.txt"
flags=(-std=gnu++11 -O1 -g -Wall -Wextra -Werror
       -Wno-conversion -Wno-sign-conversion
       -DORUN_SF5K3L_HOST_TEST -DORUN_SF5L0_BENCH_TEST_KEYS
       -DORUN_STEP3B_GATEWAY_ENROLLMENT
       -Ifirmware/include -Ifirmware/tests/sf5)
if [[ "${ORUN_HOST_SANITIZERS:-1}" == 1 ]]; then
  flags+=(-fsanitize=address,undefined -fno-sanitize-recover=undefined)
fi
echo "[3B] signed Gateway grant snapshot (Python-built, firmware-installed)"
"${CXX:-g++}" "${flags[@]}" \
  firmware/tests/sf5/test_step3b_gateway_enrollment.cpp \
  firmware/tests/sf5/sf5k3l_openssl_crypto_test_port.cpp \
  firmware/src/step3b_gateway_enrollment.cpp \
  firmware/src/gateway_grant_snapshot_reader.cpp \
  firmware/src/observation_store_format.cpp \
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
  -lcrypto -o "$dir/step3b"
"$dir/step3b" "$dir/vectors.txt"
PYTHONDONTWRITEBYTECODE=1 python3 firmware/tests/sf5/test_step3b_gateway_grants.py
