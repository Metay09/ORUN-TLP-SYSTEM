#!/usr/bin/env bash
# SF5K3AC: test-flag-only mutation of FAKE NOR, not real device flash.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/../.."
PYTHONDONTWRITEBYTECODE=1 python3 firmware/tests/sf5/test_sf5k3ac_transaction_source.py
dir="$(mktemp -d "${TMPDIR:-/tmp}/orun-sf5k3ac.XXXXXXXX")"
trap 'rm -rf -- "$dir"' EXIT
flags=(-std=gnu++11 -O1 -g -Wall -Wextra -Werror
       -DORUN_SF5K3AC_TEST_ONLY_TRANSACTION=1
       -Ifirmware/include -Ifirmware/tests/sf5)
if [[ "${ORUN_HOST_SANITIZERS:-1}" == 1 ]]; then
  flags+=(-fsanitize=address,undefined -fno-sanitize-recover=undefined)
fi
"${CXX:-g++}" "${flags[@]}" \
  firmware/tests/sf5/test_sf5k3ac_signed_append_transaction.cpp \
  firmware/src/gateway_signed_grant_append_transaction.cpp \
  firmware/src/gateway_signed_grant_append_preflight.cpp \
  firmware/src/gateway_anchored_grant_source.cpp \
  firmware/src/gateway_grant_snapshot_reader.cpp \
  firmware/src/gateway_product_startup_guard.cpp \
  firmware/src/product_custody_store.cpp \
  firmware/src/product_region.cpp \
  firmware/src/custody_store_product_format.cpp \
  firmware/src/observation_store_format.cpp \
  -lcrypto -o "$dir/transaction"
"$dir/transaction"
