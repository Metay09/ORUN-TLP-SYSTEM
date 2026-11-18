#!/usr/bin/env bash
# HOST ONLY: fake 32+7-page NOR; integrates the existing OCQ2 store.
# Does NOT touch physical firmware, device Flash, USB, DB, radio or services.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/../.."

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

g++ -std=gnu++17 -O1 -Wall -Wextra -Wpedantic -Werror \
  -fno-omit-frame-pointer -fsanitize=address,undefined \
  -DORUN_KQ05_SEGMENTED_HOST_TEST=1 \
  -I firmware/include \
  firmware/src/product_region.cpp \
  firmware/src/custody_store_product_format.cpp \
  firmware/src/observation_store_format.cpp \
  firmware/src/kq05_supplemental_28k_census.cpp \
  firmware/src/product_custody_store.cpp \
  firmware/tests/sf5/test_kq05_segmented_flash_host.cpp \
  -o "$tmp/segmented-nor-ocq2"

"$tmp/segmented-nor-ocq2"
echo "KQ05 156KiB 39-PAGE HOST-ONLY SEGMENTED STORE: PASS"
