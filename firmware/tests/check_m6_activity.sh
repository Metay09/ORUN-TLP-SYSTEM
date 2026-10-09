#!/usr/bin/env bash
set -euo pipefail

# Focused M6 activity/SF5 period-evidence verification + production build.
# Host and build only. Does not upload, erase, reset, or access the board.
cd "$(dirname "$0")/../.."

echo "=== M6 activity focused test (ASan/UBSan) ==="
g++ -std=c++17 -O1 -g -Wall -Wextra -Werror \
  -fsanitize=address,undefined -fno-sanitize-recover=undefined \
  -Ifirmware/tests/m6/stubs \
  -Ifirmware/tests/m3/stubs \
  -Ifirmware/include \
  firmware/tests/m6/test_m6_activity_capture.cpp \
  firmware/src/accelerometer_manager.cpp \
  firmware/src/i2c_recovery.cpp \
  firmware/src/activity_capture.cpp \
  firmware/src/activity_window.cpp \
  firmware/src/activity_quality.cpp \
  firmware/src/activity_auto_sampler.cpp \
  firmware/src/activity_period_evidence.cpp \
  -o /tmp/orun-m6-auto

/tmp/orun-m6-auto

echo "=== RAK4630 firmware build (NO UPLOAD) ==="
pio run -d firmware -e rak4630

echo "=== M6 ACTIVITY AND RAK4630 BUILD: PASS ==="
