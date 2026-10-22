#!/usr/bin/env bash
# Signed Gateway-only security partition candidate, TEST FAKE issuer proof.
# No live signature implementation, enrollment command, flash writes or RF.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/../.."
dir="$(mktemp -d "${TMPDIR:-/tmp}/orun-sf5k3y.XXXXXXXX")"
trap 'rm -rf -- "$dir"' EXIT
flags=(-std=gnu++11 -O1 -g -Wall -Wextra -Werror -Ifirmware/include)
if [[ "${ORUN_HOST_SANITIZERS:-1}" == 1 ]]; then
  flags+=(-fsanitize=address,undefined -fno-sanitize-recover=undefined)
fi
"${CXX:-g++}" "${flags[@]}" \
  firmware/tests/sf5/test_sf5k3y_gateway_grant_reader.cpp \
  firmware/src/gateway_grant_snapshot_reader.cpp \
  firmware/src/gateway_product_startup_guard.cpp \
  firmware/src/product_custody_store.cpp \
  firmware/src/product_region.cpp \
  firmware/src/custody_store_product_format.cpp \
  firmware/src/observation_store_format.cpp \
  -o "$dir/reader"
"$dir/reader"
