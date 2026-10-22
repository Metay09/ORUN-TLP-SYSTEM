#!/usr/bin/env bash
# OpenSSL Ed25519 host-only issuer signature + rollback checkpoint contract.
# No embedded production trust anchor or durable checkpoint backend.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/../.."
PYTHONDONTWRITEBYTECODE=1 python3 firmware/tests/sf5/test_sf5k3z_gateway_issuer_rollback_source.py
dir="$(mktemp -d "${TMPDIR:-/tmp}/orun-sf5k3z.XXXXXXXX")"
trap 'rm -rf -- "$dir"' EXIT
flags=(-std=gnu++11 -O1 -g -Wall -Wextra -Werror
       -Ifirmware/include -Ifirmware/tests/sf5)
if [[ "${ORUN_HOST_SANITIZERS:-1}" == 1 ]]; then
  flags+=(-fsanitize=address,undefined -fno-sanitize-recover=undefined)
fi
"${CXX:-g++}" "${flags[@]}" \
  firmware/tests/sf5/test_sf5k3z_gateway_issuer_rollback.cpp \
  firmware/src/gateway_anchored_grant_source.cpp \
  firmware/src/gateway_grant_snapshot_reader.cpp \
  firmware/src/gateway_product_startup_guard.cpp \
  firmware/src/product_custody_store.cpp \
  firmware/src/product_region.cpp \
  firmware/src/custody_store_product_format.cpp \
  firmware/src/observation_store_format.cpp \
  -lcrypto -o "$dir/issuer_check"
"$dir/issuer_check"
