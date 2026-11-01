#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
CXX="${CXX:-g++}"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
"$CXX" -std=c++17 -O1 -Wall -Wextra -Werror \
  -DORUN_STEP3A_REAL_TRACKER_PROVISION \
  -ffunction-sections -fdata-sections -Wl,--gc-sections \
  -I "$ROOT/firmware/include" \
  "$ROOT/firmware/src/tracker_real_enrollment.cpp" \
  "$ROOT/firmware/tests/sf5/test_phase2_real_tracker_usb.cpp" \
  -o "$TMP/test_real_tracker_usb"
"$TMP/test_real_tracker_usb"
