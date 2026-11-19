#!/usr/bin/env bash
# ORUN main-based integration: canonical 0x0A 1..5 wire only (NO RF/HMAC).
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/../.."
tmp="$(mktemp -d "${TMPDIR:-/tmp}/orun-batch-wire.XXXXXXXX")"
trap 'rm -rf -- "$tmp"' EXIT
cxx="${CXX:-g++}"
"$cxx" -std=gnu++11 -O1 -g -Wall -Wextra -Werror \
  -Ifirmware/include \
  firmware/tests/sf5/test_kq05_gateway_batch_custody_ack_wire.cpp \
  firmware/src/kq05_gateway_batch_custody_ack.cpp \
  -o "$tmp/batch_wire"
"$tmp/batch_wire"
echo "ORUN main-base 0x0A 1..5 wire: PC PASS (NO RF/FLASH/HMAC)"
