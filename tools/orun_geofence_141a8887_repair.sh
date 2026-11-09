#!/usr/bin/env bash
# One-time, exact-device, geofence-A-only service repair.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
FW="$ROOT/firmware"
PORT="/dev/serial/by-id/usb-RAKwireless_WisCore_RAK4631_Board_52E16773141A8887-if00"
REPAIR_ENV=rak4630_orun_fence_repair_141a8887
NORMAL_ENV=rak4630_orun
PIO="$(command -v pio || true)"
[[ -n "$PIO" ]] || PIO="$HOME/.platformio/penv/bin/pio"
[[ -x "$PIO" && -e "$PORT" && -r "$HOME/orun-issuer.pub" ]] || {
  echo "DUR: PlatformIO, USB tasma ya da issuer public key bulunamadi"
  exit 1
}
if fuser -s "$PORT" 2>/dev/null; then
  echo "DUR: Tasma USB portunu baska program kullaniyor"; exit 1
fi
TMP="$(mktemp -d)"
trap 'rm -rf -- "$TMP"' EXIT

read_identity() {
  python3 "$ROOT/tools/orun_cihaz_kontrol.py" oku --port "$PORT" \
    --out "$1" --deneme 5
}
verify_fence() {
  python3 - "$PORT" "$1" <<'PY'
import serial, sys, time
port, phase = sys.argv[1:]
responses = []
with serial.Serial(port, 115200, timeout=0.25,
                   write_timeout=2, exclusive=True) as ser:
    ser.dtr = True
    ser.rts = False
    time.sleep(1)
    for cmd, prefix, ending in (
        ("APP FENCE SERVICE?", "APP FENCE SERVICE ", "APP FENCE SERVICE "),
        ("APP FENCE DIAG?", "APP FENCE DIAG ", "APP FENCE DIAG maintenance="),
        ("APP GEOFENCE?", "APP GEOFENCE id=", "APP GEOFENCE id="),
        ("APP STORAGE?", "APP STORAGE id=", "APP STORAGE id="),
    ):
        ser.reset_input_buffer()
        ser.write((cmd + "\n").encode())
        ser.flush()
        found = False
        end = time.monotonic() + 12
        while time.monotonic() < end:
            line = ser.readline().decode(errors="replace").strip()
            if line.startswith(prefix):
                print(line, flush=True)
                responses.append(line)
                if line.startswith(ending):
                    found = True
                    break
        if not found:
            raise SystemExit("DUR: Cevap yok: " + cmd)
service = next((s for s in responses if s.startswith("APP FENCE SERVICE ")), "")
a = next((s for s in responses if "page=A " in s), "")
b = next((s for s in responses if "page=B " in s), "")
m = next((s for s in responses if s.startswith("APP FENCE DIAG maintenance=")), "")
g = next((s for s in responses if s.startswith("APP GEOFENCE id=")), "")
s = next((s for s in responses if s.startswith("APP STORAGE id=")), "")
def require(ok, reason):
    if not ok: raise SystemExit("DUR: " + reason)
if phase == "before":
    require("repair=no" in service, "Onarim oncesinde normal teshis image gerekli")
    require("evidence=SUPPORTED_CORRUPT" in a and "all_ff=no" in a and "tail_ff=no" in a, "A beklenen bozuk durumunda degil")
    require("evidence=ERASED" in b and "all_ff=yes" in b and "tail_ff=yes" in b, "B bos degil")
    require("maintenance=yes" in m and "geofence_maintenance=yes" in s, "Bakim kilidi yok")
else:
    require(("repair=yes" if phase == "repaired" else "repair=no") in service, "Yanlis image")
    require("evidence=COMMITTED_CLEAR" in a and "tail_ff=yes" in a, "A saglam CLEAR degil")
    require("evidence=ERASED" in b and "all_ff=yes" in b and "tail_ff=yes" in b, "B degisti")
    require("maintenance=no" in m and "geofence_maintenance=no geofence_corrupt=0" in s, "Bakim kilidi devam ediyor")
    require("resource=1 token=1 areas=0 vertices=0 runtime=clear" in g, "Alan CLEAR ve VALID degil")
print("DOGRULANDI: " + phase, flush=True)
PY
}
do_build_upload() {
  local env="$1" label="$2"
  echo "=== $label DERLENIYOR ==="
  if ! "$PIO" run -d "$FW" -e "$env" > "$TMP/$label-build.log" 2>&1; then
    tail -n 60 "$TMP/$label-build.log"; return 1
  fi
  grep -E '^(RAM|Flash):|SUCCESS|ERROR' "$TMP/$label-build.log" | tail -n 7 || true
  echo "=== $label YUKLENIYOR ==="
  if ! "$PIO" run -d "$FW" -e "$env" -t upload \
      --upload-port "$PORT" > "$TMP/$label-upload.log" 2>&1; then
    tail -n 60 "$TMP/$label-upload.log"; return 1
  fi
  sleep 4
}

echo "=== 141A8887 KIMLIK DOGRULAMA ==="
read_identity "$TMP/before.json"
python3 - "$TMP/before.json" <<'PY'
import json,sys
s=json.load(open(sys.argv[1]))
assert s.get("kind")=="tracker" and s.get("id")=="52E16773141A8887", "Yanlis cihaz"
assert s.get("credential") not in (None,"none") and s.get("test_key") is False, "Guvenlik durumu uygunsuz"
assert s.get("store_ready")=="yes" and s.get("store_faulted")=="no", "Kayit depolama uygun degil"
print("DOGRU: cihaz, guvenlik ve kayitlar")
PY

echo "=== A/B FIZIKSEL KOSULLAR ==="
verify_fence before

echo "YALNIZ BU TASMANIN BOZUK ALAN SAYFASI A SILINECEK."
echo "Guvenlik, konum gecmisi, cihaz ayarlari, gateway izni korunacak."
echo "Alan tanimi bos (CLEAR) olarak yeniden olusturulacak."
read -r -p "Onayliyorsan ONAR yaz: " answer
[[ "$answer" == "ONAR" ]] || { echo "Iptal edildi"; exit 0; }

echo "=== DIAGNOSTIK KAYNAK VE LORA TESTLERI ==="
python3 "$FW/tests/m6/test_geofence_readonly_diagnostic_source.py"
python3 "$FW/tests/m6/test_geofence_141a8887_repair_source.py"
bash "$ROOT/tools/ci/run_sf5l0_bench_round_trip_host.sh"

do_build_upload "$REPAIR_ENV" "REPAIR" || {
  echo "DUR: Servis yazilimi basarisiz; cihaz durumunu tekrar incele"; exit 1
}
echo "=== REPAIR SONRASI KIMLIK/KAYITLAR ==="
read_identity "$TMP/after_repair.json"
python3 "$ROOT/tools/orun_cihaz_kontrol.py" karsilastir \
  "$TMP/before.json" "$TMP/after_repair.json"
verify_fence repaired

do_build_upload "$NORMAL_ENV" "NORMAL" || {
  echo "DUR: Servis image'i cihazda kalmis olabilir; inceleme gerekli"; exit 1
}
echo "=== SON KONTROLLER ==="
read_identity "$TMP/after_normal.json"
python3 "$ROOT/tools/orun_cihaz_kontrol.py" karsilastir \
  "$TMP/before.json" "$TMP/after_normal.json"
verify_fence final

echo "=== BASARILI: 141A8887 geofence yeniden olusturuldu ==="
echo "Normal yazilim geri yuklendi; kayitlar ve anahtarlar korundu."
echo "Alan tanimi bilerek CLEAR olarak sifirlandi."
