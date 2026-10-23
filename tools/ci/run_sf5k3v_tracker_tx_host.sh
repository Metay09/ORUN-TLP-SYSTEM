#!/usr/bin/env bash
# Pilot fake sealer demonstrates custody cache sequencing, NOT AEAD or on-air RX.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/../.."
PYTHONDONTWRITEBYTECODE=1 python3 firmware/tests/sf5/test_sf5k3v_tracker_tx_source.py
dir="$(mktemp -d "${TMPDIR:-/tmp}/orun-sf5k3v.XXXXXXXX")"
trap 'rm -rf -- "$dir"' EXIT
cxx="${CXX:-g++}"
flags=(-std=gnu++11 -O1 -g -Wall -Wextra -Werror
       -Ifirmware/include -Ifirmware/tests/sf5)
if [[ "${ORUN_HOST_SANITIZERS:-1}" == 1 ]]; then
  flags+=(-fsanitize=address,undefined -fno-sanitize-recover=undefined)
fi
"$cxx" "${flags[@]}" \
  firmware/tests/sf5/test_sf5k3v_tracker_pilot.cpp \
  firmware/src/tracker_product_tx_pilot.cpp \
  firmware/src/product_record_store.cpp firmware/src/product_region.cpp \
  firmware/src/observation_store.cpp firmware/src/observation_store_control.cpp \
  firmware/src/observation_store_format.cpp \
  firmware/src/product_period_builder.cpp firmware/src/tlp_v2_product_secure.cpp \
  -o "$dir/pilot"
"$dir/pilot"
