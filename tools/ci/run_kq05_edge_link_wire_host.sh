#!/usr/bin/env bash
# KQ05 isolated native byte-layout and SF5L6 BLE 20-byte framing test.
# No device access, radio transmission, firmware upload or default-role change.
set -euo pipefail
cd "$(dirname "$0")/../.."

tmp_dir="$(mktemp -d /tmp/orun-kq05-edgewire.XXXXXX)"
cleanup() {
  local status=$?
  if [[ -n "$tmp_dir" && -d "$tmp_dir" && ! -L "$tmp_dir" ]]; then
    rm -rf -- "$tmp_dir"
  fi
  return "$status"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

g++ -std=c++17 -O1 -g -Wall -Wextra -Werror \
  -DORUN_KQ05_EXPERIMENTAL_ACKLESS_LIVE \
  -Ifirmware/include \
  firmware/src/edge_link.cpp \
  firmware/src/kq05_edge_link_wire.cpp \
  firmware/tests/sf5/test_kq05_edge_link_wire.cpp \
  -o "$tmp_dir/kq05_edge_wire"

"$tmp_dir/kq05_edge_wire"
echo "KQ05 native BLE 20B fragment + logical offer/staged layout: PASS (HOST ONLY)"
