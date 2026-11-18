#!/usr/bin/env bash
# KQ05 bounded off-line two-store routing policy, PC only.
# No device ports, Flash mutation, DB, services, radio or production ACK.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/../.."

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

g++ -std=gnu++17 -O2 -Wall -Wextra -Wpedantic -Werror \
  -fno-omit-frame-pointer -fsanitize=undefined \
  -I firmware/include \
  firmware/tests/sf5/test_kq05_dual_region_decisions.cpp \
  -o "$tmp/dual-region-decisions"

"$tmp/dual-region-decisions"
echo "KQ05 DUAL REGION ROUTING POLICY: PC PASS"
