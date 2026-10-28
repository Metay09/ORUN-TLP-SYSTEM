#!/usr/bin/env bash
# Test-only observation-region reset host gate. No physical erase happens here.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/../.."
out="$(mktemp -d "${TMPDIR:-/tmp}/orun-bench-clear.XXXXXXXX")"
trap 'rm -rf -- "$out"' EXIT
cxx="${CXX:-g++}"
flags=(-std=gnu++11 -O1 -g -Wall -Wextra -Werror
       -DORUN_SF5L0_TRACKER=1 -Ifirmware/include)
if [[ "${ORUN_HOST_SANITIZERS:-1}" == 1 ]]; then
  flags+=(-fsanitize=address,undefined -fno-sanitize-recover=undefined)
fi
"$cxx" "${flags[@]}" \
  firmware/tests/sf5/test_sf5f_product_record_store.cpp \
  firmware/src/product_record_store.cpp firmware/src/product_region.cpp \
  firmware/src/observation_store.cpp firmware/src/observation_store_control.cpp \
  firmware/src/observation_store_format.cpp \
  firmware/src/product_period_builder.cpp firmware/src/tlp_v2_product_secure.cpp \
  -o "$out/bench_clear"
"$out/bench_clear" --bench-clear-only

PYTHONDONTWRITEBYTECODE=1 python3 - <<'PY'
from pathlib import Path
app = Path("firmware/src/main.cpp").read_text()
hdr = Path("firmware/include/product_record_store.h").read_text()
store = Path("firmware/src/product_record_store.cpp").read_text()
command = "BENCH TRACKER ERASE RECORDS CONFIRM"
assert app.count(command) == 1
begin = app.index('if (isActivityCommand("BENCH DRAIN OFF", 15))')
end = app.index("#endif", begin)
scope = app[begin:end]
for guard in ("!product_records.requestBenchClear()", "tracking_enabled",
              "bench_pacer.enabled()", "bench_pacer.awaitingAck()",
              "custody_ack_release.pending()"):
    assert guard in scope, guard
assert "#ifdef ORUN_SF5L0_TRACKER" in hdr and "requestBenchClear()" in hdr
assert "#ifdef ORUN_SF5L0_TRACKER" in store
assert "requestRegionErase(false)" in store
assert "requestRegionErase(true)" in store
assert "product_region::Owner::kObservation" in store
print("SF5L0 bench clear source scope: PASS")
PY
