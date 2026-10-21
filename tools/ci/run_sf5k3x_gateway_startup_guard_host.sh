#!/usr/bin/env bash
# Gateway boot fail-closed preflight. Fake NOR, actual OCQ2 store and census.
# No credential commissioning or real SX1262 RF; no physical flash upload.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/../.."
PYTHONDONTWRITEBYTECODE=1 python3 firmware/tests/sf5/test_sf5k3x_gateway_source.py
dir="$(mktemp -d "${TMPDIR:-/tmp}/orun-sf5k3x.XXXXXXXX")"
trap 'rm -rf -- "$dir"' EXIT
flags=(-std=gnu++11 -O1 -g -Wall -Wextra -Werror -Ifirmware/include)
if [[ "${ORUN_HOST_SANITIZERS:-1}" == 1 ]]; then
  flags+=(-fsanitize=address,undefined -fno-sanitize-recover=undefined)
fi
"${CXX:-g++}" "${flags[@]}" \
  firmware/tests/sf5/test_sf5k3x_gateway_startup_guard.cpp \
  firmware/src/gateway_product_startup_guard.cpp \
  firmware/src/product_custody_store.cpp \
  firmware/src/product_region.cpp \
  firmware/src/custody_store_product_format.cpp \
  -o "$dir/gateway_boot"
"$dir/gateway_boot"
