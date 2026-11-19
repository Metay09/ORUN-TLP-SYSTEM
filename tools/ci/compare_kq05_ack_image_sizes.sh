#!/usr/bin/env bash
# Compile BOTH isolated lab images; no pio upload, physical Flash, or erase.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/../../firmware"
command -v pio >/dev/null || {
  echo "DUR: PlatformIO (pio) bulunamadi."
  exit 1
}
pio run -e rak4630_kq05_lab
pio run -e rak4630_kq05_unified_ack_lab
python3 - <<'PY'
from pathlib import Path
one = Path(".pio/build/rak4630_kq05_lab/firmware.bin")
two = Path(".pio/build/rak4630_kq05_unified_ack_lab/firmware.bin")
for p in (one, two):
    if not p.is_file():
        raise SystemExit("DUR: Eksik firmware image: " + str(p))
baseline, unified = one.stat().st_size, two.stat().st_size
if baseline <= 0 or unified <= 0:
    raise SystemExit("DUR: Bos firmware image")
print("KQ05 temel LAB image     :", baseline, "bytes")
print("KQ05 birlesik ACK image :", unified, "bytes")
print("Toplam flash farki       :", unified-baseline, "bytes",
      f"({(unified/baseline-1)*100:+.2f}%)")
print("Not: fark tum opt-in ozellikleri kapsar, yalniz 0x05 refactor kazanci degil.")
print("Cihaz yukleme / erase YOK.")
PY
