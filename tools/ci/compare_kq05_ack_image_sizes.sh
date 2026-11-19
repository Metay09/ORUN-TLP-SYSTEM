#!/usr/bin/env bash
# Compile BOTH isolated lab images; no pio upload, physical Flash, or erase.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/../../firmware"
command -v pio >/dev/null || {
  echo "DUR: PlatformIO (pio) bulunamadi."
  exit 1
}
# Do not flood the terminal with thousands of compiler progress lines.
# Preserve complete logs for each build inside a temporary PC file; on
# failure show diagnostics and stop before comparing nonexistent artifacts.
log="$(mktemp "${TMPDIR:-/tmp}/orun-kq05-ack-size.XXXXXXXX")"
trap 'rm -f -- "$log"' EXIT
for target in rak4630_kq05_lab rak4630_kq05_unified_ack_lab; do
  echo "=== BUILD $target ==="
  if ! pio run -e "$target" >"$log" 2>&1; then
    echo "DUR: Firmware derlemesi basarisiz: $target"
    grep -En '(^|[[:space:]])(error:|fatal error:)|Error [0-9]|FAILED' "$log" | tail -n 40 || true
    echo "=== SON 80 DERLEME SATIRI ==="
    tail -n 80 "$log"
    exit 1
  fi
  # PlatformIO memory and status summary, without losing build warnings.
  grep -E '(^RAM:|^Flash:|\[SUCCESS\]|SUCCESS|warning:)' "$log" | tail -n 25 || true
done
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
