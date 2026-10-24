#!/usr/bin/env bash
# Custody-path store scan cost (one data pass per snapshot/count) + CRC-32.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/../.."
dir="$(mktemp -d "${TMPDIR:-/tmp}/orun-sf5scan.XXXXXXXX")"
trap 'rm -rf -- "$dir"' EXIT
cxx="${CXX:-g++}"
flags=(-std=gnu++11 -O1 -g -Wall -Wextra -Werror
       -Ifirmware/include -Ifirmware/tests/sf5)
if [[ "${ORUN_HOST_SANITIZERS:-1}" == 1 ]]; then
  flags+=(-fsanitize=address,undefined -fno-sanitize-recover=undefined)
fi
"$cxx" "${flags[@]}" \
  firmware/tests/sf5/test_sf5_store_scan_cost.cpp \
  firmware/src/product_record_store.cpp firmware/src/product_region.cpp \
  firmware/src/observation_store.cpp firmware/src/observation_store_control.cpp \
  firmware/src/observation_store_format.cpp \
  firmware/src/product_period_builder.cpp firmware/src/tlp_v2_product_secure.cpp \
  -o "$dir/scan"
"$dir/scan"
