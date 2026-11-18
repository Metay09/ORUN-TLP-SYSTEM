#!/usr/bin/env bash
# KQ05 supplemental 28 KiB: PORTABLE READ-ONLY census test only.
# No physical Flash, USB/BLE, systemd, SQLite, radio or firmware upload.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/../.."

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

g++ -std=gnu++11 -O1 -Wall -Wextra -Wpedantic -Werror \
  -fno-omit-frame-pointer -fsanitize=address,undefined \
  -I firmware/include \
  firmware/src/observation_store_format.cpp \
  firmware/src/custody_store_product_format.cpp \
  firmware/src/kq05_supplemental_28k_census.cpp \
  firmware/tests/sf5/test_kq05_supplemental_28k_census.cpp \
  -o "$tmp/supplemental-28k-census"

"$tmp/supplemental-28k-census"
echo "KQ05 28KiB ROLE / BOUNDARY READ-ONLY HOST GATE: PASS"
