#!/usr/bin/env bash
# KQ05 Gateway selective batch custody ACK structural codec - PC ONLY.
# Does NOT sign, verify HMAC, release, send RF or touch any real NOR.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/../.."
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

g++ -std=gnu++17 -O1 -Wall -Wextra -Wpedantic -Werror \
  -fno-omit-frame-pointer -fsanitize=address,undefined \
  -Ifirmware/include \
  firmware/src/kq05_gateway_batch_custody_ack.cpp \
  firmware/tests/sf5/test_kq05_gateway_batch_custody_ack_wire.cpp \
  -o "$tmp/kq05-gateway-batch-ack"

"$tmp/kq05-gateway-batch-ack"
echo "KQ05 GATEWAY SELECTIVE BATCH ACK WIRE: PC PASS"
