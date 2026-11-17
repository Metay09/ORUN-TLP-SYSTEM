#!/usr/bin/env bash
# Clean-slate P0: isolated real ProductRecordStore EVENT+PERIODIC NOR regression.
# PC only; neither firmware upload nor persistent data/DB mutation.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/../.."
test -z "$(git status --porcelain --untracked-files=no)" || {
  echo "DUR: uncommitted tracked sources." >&2
  exit 2
}

OUT="$(mktemp /tmp/orun-kq05-event-store.XXXXXX)"
trap 'rm -f -- "$OUT"' EXIT

g++ -std=gnu++11 -O1 -g -Wall -Wextra -Werror \
  -fsanitize=address,undefined -fno-sanitize-recover=undefined \
  -Ifirmware/include \
  firmware/tests/sf5/test_sf5f_product_record_store.cpp \
  firmware/src/product_record_store.cpp \
  firmware/src/product_region.cpp \
  firmware/src/observation_store.cpp \
  firmware/src/observation_store_control.cpp \
  firmware/src/observation_store_format.cpp \
  firmware/src/product_period_builder.cpp \
  firmware/src/tlp_v2_product_secure.cpp \
  -o "$OUT"
"$OUT"
echo "KQ05 EVENT+PERIODIC store native regression: PASS (PC fake NOR)"
echo "NO DEVICE FLASH, NO DB OR SERVICE ACTION, NOT FIELD RELEASE"
