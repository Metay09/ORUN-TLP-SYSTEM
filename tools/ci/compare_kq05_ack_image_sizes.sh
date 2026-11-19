#!/usr/bin/env bash
# Compile both isolated KQ05 RAK images, compare PlatformIO linker memory
# reports. Nordic build does NOT guarantee a firmware.bin; ELF / package
# formats are target-specific, so a missing .bin is not a build failure.
# No pio upload, no device Flash, erase, or production merge.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/../../firmware"
command -v pio >/dev/null || {
  echo "DUR: PlatformIO (pio) bulunamadi."
  exit 1
}
command -v python3 >/dev/null || {
  echo "DUR: python3 bulunamadi."
  exit 1
}

tmp="$(mktemp -d "${TMPDIR:-/tmp}/orun-kq05-ack-size.XXXXXXXX")"
trap 'rm -rf -- "$tmp"' EXIT

for target in rak4630_kq05_lab rak4630_kq05_unified_ack_lab; do
  log="$tmp/$target.log"
  echo "=== BUILD $target ==="
  if ! pio run -e "$target" >"$log" 2>&1; then
    echo "DUR: Firmware derlemesi basarisiz: $target"
    grep -En '(^|[[:space:]])(error:|fatal error:)|Error [0-9]|FAILED' "$log" | tail -n 40 || true
    echo "=== SON 80 DERLEME SATIRI ==="
    tail -n 80 "$log"
    exit 1
  fi
  # Show the same authoritative memory figures as PlatformIO's build report.
  grep -E '(^RAM:|^Flash:|\[SUCCESS\]|SUCCESS)' "$log" | tail -n 8 || true
done

python3 - "$tmp/rak4630_kq05_lab.log" "$tmp/rak4630_kq05_unified_ack_lab.log" <<'PY'
from pathlib import Path
import re
import sys

MEMORY = re.compile(
    r"^(RAM|Flash):[^\n]*used\s+(\d+)\s+bytes\s+from\s+(\d+)\s+bytes",
    re.MULTILINE,
)

def usage(path):
    path = Path(path)
    lines = path.read_text(encoding="utf-8", errors="replace")
    matched = {}
    for match in MEMORY.finditer(lines):
        matched[match.group(1)] = (int(match.group(2)), int(match.group(3)))
    if set(matched) != {"RAM", "Flash"}:
        raise SystemExit(
            "DUR: PlatformIO memory ozeti eksik; "
            f"{path.name}: {list(matched)}"
        )
    return matched

baseline, unified = [usage(p) for p in sys.argv[1:]]
print("=== KQ05 FIRMWARE BELLEK KARSILASTIRMASI ===")
for kind in ("Flash", "RAM"):
    old, old_limit = baseline[kind]
    new, new_limit = unified[kind]
    if old_limit != new_limit or old_limit == 0:
        raise SystemExit(f"DUR: {kind} kapasitesi farkli; karsilastirma gecersiz.")
    delta = new - old
    print(f"{kind}: temel={old:,} B ({old / old_limit:.1%}), "
          f"birlesik={new:,} B ({new / old_limit:.1%}), "
          f"fark={delta:+,} B")
print("Bu, iki farkli ozellik setine sahip LAB image karsilastirmasidir.")
print("Tek basina ACK refactor flash kazancini izole etmez.")
print("Cihaz upload / erase YOK.")
PY
