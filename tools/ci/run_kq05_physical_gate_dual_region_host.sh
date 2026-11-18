#!/usr/bin/env bash
# PC HOST ONLY: real FlashMutationGate/NrfProductFlash/NrfHistoryFlash
# with Nordic SVC stubs and mmap'd synthetic physical flash addresses.
# Never opens USB, flashes devices, erases physical NOR or migrates History.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/../.."

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

g++ -std=gnu++17 -O1 -Wall -Wextra -Wpedantic -Werror \
  -fno-pie -no-pie -fno-omit-frame-pointer \
  -Ifirmware/tests/m4/nrf_stubs -Ifirmware/include \
  -Wl,--defsym,__flash_arduino_end=0xED000 \
  -DORUN_KQ05_SEGMENTED_HOST_TEST=1 \
  firmware/tests/sf5/test_kq05_physical_gate_dual_region_host.cpp \
  firmware/src/flash_mutation_gate.cpp \
  firmware/src/nrf_history_flash.cpp \
  firmware/src/nrf_product_flash.cpp \
  firmware/src/nrf_config_flash.cpp \
  firmware/src/nrf_security_flash.cpp \
  firmware/src/nrf_geofence_flash.cpp \
  firmware/src/kq05_supplemental_28k_census.cpp \
  firmware/src/observation_store_format.cpp \
  firmware/src/custody_store_product_format.cpp \
  -o "$tmp/kq05-physical-gate"

"$tmp/kq05-physical-gate"
echo "KQ05 PHYSICAL 128+28KiB GATE / SHARED OWNER: PC PASS"
