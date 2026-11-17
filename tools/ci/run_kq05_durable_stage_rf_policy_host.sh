#!/usr/bin/env bash
# KQ05 PC-only: durable Backend->Edge->Gateway 0x92 stage result to native
# radio-candidate priority/budget gate. NO live RF or device/service writes.
set -euo pipefail
cd "$(dirname "$0")/../.."

tmp="$(mktemp -d "${TMPDIR:-/tmp}/orun-kq05-durable-rf.XXXXXXXX")"
trap 'rm -rf -- "$tmp"' EXIT

PYTHONDONTWRITEBYTECODE=1 python3 \
  firmware/tests/sf5/generate_kq05_durable_rf_fixture.py \
  "$tmp/offer.bin" "$tmp/staged.bin"

flags=(-std=c++17 -O1 -g -Wall -Wextra -Werror
       -DORUN_KQ05_EXPERIMENTAL_ACKLESS_LIVE
       -Ifirmware/include -Ifirmware/tests/sf5)
if [[ "${ORUN_HOST_SANITIZERS:-1}" == 1 ]]; then
  flags+=(-fsanitize=address,undefined -fno-sanitize-recover=undefined)
fi

"${CXX:-g++}" "${flags[@]}" \
  firmware/src/kq05_gateway_ram_relay.cpp \
  firmware/src/kq05_edge_link_wire.cpp \
  firmware/src/edge_link.cpp \
  firmware/tests/sf5/sf5k3l_openssl_crypto_test_port.cpp \
  firmware/tests/sf5/test_kq05_durable_stage_rf_policy.cpp \
  -lcrypto -o "$tmp/receipt-policy"

"$tmp/receipt-policy" "$tmp/offer.bin" "$tmp/staged.bin"
echo "NO RADIO TX, NO DEVICE FLASH, NO DB/SERVICE MUTATION"
