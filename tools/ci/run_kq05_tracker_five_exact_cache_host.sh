#!/usr/bin/env bash
# KQ05 Tracker OBS1 durable cache 4 -> 5 host regression.
# No target image modifications, physical erase or real device uploads.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/../.."
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

g++ -std=gnu++11 -O1 -g -Wall -Wextra -Werror \
  -Wno-conversion -Wno-sign-conversion \
  -Ifirmware/include \
  firmware/tests/sf5/test_sf5c_observation_store_controls.cpp \
  firmware/src/observation_store.cpp \
  firmware/src/observation_store_control.cpp \
  firmware/src/observation_store_format.cpp \
  -o "$tmp/tracker-five-exact"

"$tmp/tracker-five-exact"
echo "KQ05 TRACKER FIVE EXACT FLASH CACHED OBJECTS / REBOOT / SIXTH DENIAL: PC PASS"
