#!/usr/bin/env bash
# KQ05 critical EVENT handoff to EXISTING custody-ACK. PC fake NOR and RF.
# No hardware flash, no test key provisioning, no service/DB mutation.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/../.."

dir="$(mktemp -d "${TMPDIR:-/tmp}/orun-kq05-critical-event.XXXXXXXX")"
trap 'rm -rf -- "$dir"' EXIT

cxx="${CXX:-g++}"
flags=(-std=gnu++11 -O1 -g -Wall -Wextra -Werror
  -Ifirmware/include -Ifirmware/tests/sf5)
if [[ "${ORUN_HOST_SANITIZERS:-1}" == 1 ]]; then
  flags+=(-fsanitize=address,undefined -fno-sanitize-recover=undefined)
fi

"$cxx" "${flags[@]}" \
  firmware/tests/sf5/test_kq05_critical_event_tx.cpp \
  firmware/src/tracker_critical_event_tx.cpp \
  firmware/src/tracker_product_tx_pilot.cpp \
  firmware/src/tracker_ackless_live_tx.cpp \
  firmware/src/product_record_store.cpp firmware/src/product_region.cpp \
  firmware/src/observation_store.cpp firmware/src/observation_store_control.cpp \
  firmware/src/observation_store_format.cpp \
  firmware/src/product_period_builder.cpp firmware/src/tlp_v2_product_secure.cpp \
  -o "$dir/event"

"$dir/event"
echo "NO DEVICE FLASH, NO SERVICE/DB ACTION, NOT FIELD RELEASE"
