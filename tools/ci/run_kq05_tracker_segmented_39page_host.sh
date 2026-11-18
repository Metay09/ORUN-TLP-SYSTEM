#!/usr/bin/env bash
# KQ05 Tracker OBS1 through host-only 39-page segmented NOR; no device access.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/../.."
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

g++ -std=gnu++17 -O1 -Wall -Wextra -Wpedantic -Werror \
  -fno-omit-frame-pointer -fsanitize=address,undefined \
  -DORUN_KQ05_SEGMENTED_HOST_TEST=1 \
  -I firmware/include \
  firmware/src/observation_store.cpp \
  firmware/src/observation_store_control.cpp \
  firmware/src/observation_store_format.cpp \
  firmware/tests/sf5/test_kq05_tracker_segmented_39page_host.cpp \
  -o "$tmp/tracker-segmented-obs1"

"$tmp/tracker-segmented-obs1"
echo "KQ05 TRACKER 39-PAGE SINGLE SEQUENCE / FLASH RECOVERY: PC PASS"
