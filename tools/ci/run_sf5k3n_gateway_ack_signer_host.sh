#!/usr/bin/env bash
# Run from repo root; uses REAL firmware product codec/ACK parser and the
# independent OpenSSL host-only crypto provider (never linked on RAK4631).
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/../.."
work="$(mktemp -d /tmp/orun-sf5k3n.XXXXXX)"
trap 'rm -rf "$work"' EXIT
"${CXX:-g++}" -std=c++17 -O1 -g -Wall -Wextra -Werror -DORUN_SF5K3N_HOST_TEST \
 -I firmware/include \
 firmware/src/tlp_v2_product_secure.cpp \
 firmware/src/custody_ack.cpp \
 firmware/src/gateway_custody_ack_signer.cpp \
 firmware/tests/sf5/sf5k3l_openssl_crypto_test_port.cpp \
 firmware/tests/sf5/test_sf5k3n_gateway_ack_signer.cpp \
 -lssl -lcrypto -o "$work/test_sf5k3n"
"$work/test_sf5k3n"
