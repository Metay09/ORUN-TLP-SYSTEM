#!/usr/bin/env bash
# Native PC-only OCQ2/v4 Gateway device-bound header and 39-page integration.
# No device access, firmware upload, Flash erase, migration, keys or ACK.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/../.."

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

g++ -std=gnu++17 -O1 -Wall -Wextra -Wpedantic -Werror \
  -fno-omit-frame-pointer -fsanitize=address,undefined \
  -I firmware/include \
  firmware/src/custody_store_product_format.cpp \
  firmware/tests/sf5/test_kq05_gateway_bound_ocq2_header.cpp \
  -o "$tmp/bound-ocq2-header"

"$tmp/bound-ocq2-header"
bash tools/ci/run_kq05_supplemental_28k_census_host.sh
bash tools/ci/run_kq05_segmented_156k_host.sh
echo "KQ05 GATEWAY OCQ2/v4 SUPPLEMENTAL DEVICE BINDING: PC PASS"
