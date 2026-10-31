#!/usr/bin/env bash
# ORUN Phase 2 one-device Tracker DFU. No bootloader or storage erase commands.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
ID="52E16773141A8887"
PORT="/dev/serial/by-id/usb-RAKwireless_WisCore_RAK4631_Board_"$ID"-if00"
BASELINE="$(mktemp)"
LOG="$HOME/orun-phase2-tracker-upload.log"
trap 'rm -f "$BASELINE"' EXIT

if [ ! -e "$PORT" ]; then
  echo "DUR: Yeni Tracker USB'de yok."
  exit 2
fi
if fuser -s "$PORT"; then
  echo "DUR: Tracker portunu baska surec kullaniyor."
  exit 2
fi
if ! git -C "$ROOT" diff --quiet || ! git -C "$ROOT" diff --cached --quiet; then
  echo "DUR: Faz 2 kaynak klasorunde commitlenmemis degisiklik var."
  exit 2
fi
if ! git -C "$ROOT" ls-files --error-unmatch firmware/src/tracker_real_enrollment.cpp >/dev/null 2>&1; then
  echo "DUR: Gercek anahtar firmware dosyasi eksik."
  exit 2
fi

inspect() {
  python3 - "$1" "$PORT" "$BASELINE" "$ID" <<'PY'
import json, pathlib, re, serial, sys, time

mode, port, baseline, expected_id = sys.argv[1:]
for attempt in range(15 if mode == "post" else 1):
    try:
        with serial.Serial(port, 115200, timeout=0.25,
                           write_timeout=2, exclusive=True) as ser:
            ser.rts = False
            ser.dtr = True
            time.sleep(0.8)
            def ask(command, prefix):
                ser.reset_input_buffer()
                ser.write((command + "\n").encode("ascii"))
                ser.flush()
                result = []
                deadline = time.monotonic() + 4
                while time.monotonic() < deadline:
                    line = ser.readline().decode("utf8", "replace").strip()
                    if line.startswith(prefix):
                        result.append(line)
                if not result:
                    raise RuntimeError("Yanitsiz: " + command)
                return result[-1]

            role = ask("ROLE?", "ROLE ")
            ser.reset_input_buffer()
            ser.write(b"PRODUCT STORE?\n")
            ser.flush()
            storage_lines = []
            end = time.monotonic() + 5
            while time.monotonic() < end:
                line = ser.readline().decode("utf8", "replace").strip()
                if line.startswith("PRODUCT STORE "):
                    storage_lines.append(line)
            region = next((x for x in storage_lines if
                           x.startswith("PRODUCT STORE region=")), "")
            detail = next((x for x in storage_lines if
                           x.startswith("PRODUCT STORE ready=")), "")
            app = ask("APP STORAGE?", "APP STORAGE ")
            if mode == "post":
                path = ask("PRODUCT PATH?", "PRODUCT PATH ")
                bench = ask("BENCH?", "BENCH TRACKER ")
            else:
                path, bench = "", ""
        break
    except (OSError, serial.SerialException, RuntimeError) as exc:
        if attempt == (14 if mode == "post" else 0):
            raise SystemExit(f"DUR: USB kontrol edilemedi: {exc}")
        time.sleep(2)

def fields(line):
    return dict(re.findall(r"(\w+)=([^\s]+)", line))

r, d, a = fields(region), fields(detail), fields(app)
print("=== " + mode.upper() + " TRACKER ===")
for line in (role, region, detail, app):
    print(line)
if not role.startswith("ROLE TRACKER "):
    raise SystemExit("DUR: Tracker rolu degisti")
if not (r.get("region") == "OK" and r.get("decision") == "MOUNT" and
        r.get("mounted") == "yes"):
    raise SystemExit("DUR: Product Store mount bozuk")
if not (d.get("ready") == "yes" and d.get("faulted") == "no"):
    raise SystemExit("DUR: Product Store hazir degil")
if a.get("security_state") != "0":
    raise SystemExit("DUR: SecurityStore zaten provisioned/fault; durduruldu")
inc = d.get("inc")
if not inc or not re.fullmatch("[0-9A-F]{16}", inc):
    raise SystemExit("DUR: Product Store incarnation bilinmiyor")
now = {"inc": inc, "retained": int(d["retained"]),
       "next_seq": int(d["next_seq"])}
if mode == "pre":
    pathlib.Path(baseline).write_text(json.dumps(now))
    print("PRECHECK PASS: product incarnation", inc)
else:
    old = json.loads(pathlib.Path(baseline).read_text())
    print(path)
    print(bench)
    b = fields(bench)
    p = fields(path)
    if b.get("id") != expected_id:
        raise SystemExit("DUR: USB yanlis cihaz kimligi")
    if b.get("security") != "UNPROVISIONED":
        raise SystemExit("DUR: SecurityStore onceden degismis")
    if p.get("image") != "TX_PILOT" or \
       p.get("secure_tx") != "ONE_PER_BOOT":
        raise SystemExit("DUR: Faz 2 pilot imaji dogrulanamadi")
    if now["inc"] != old["inc"]:
        raise SystemExit("DUR: KAYIT KIMLIGI DEGISMIS; provision YAPMA")
    if now["next_seq"] < old["next_seq"] or \
       now["retained"] < old["retained"]:
        raise SystemExit("DUR: KAYIT SAYISI AZALMIS; provision YAPMA")
    print("POSTCHECK PASS: urun kayitlari ve SecurityStore korundu.")
    print("Gercek anahtar USB'ye HENUZ GONDERILMEDI.")
PY
}

echo "=== YALNIZ YENI TRACKER / ILK KORUMA KONTROLU ==="
inspect pre
echo "=== GERCEK ANAHTAR USB HOST TESTI ==="
bash "$ROOT/tools/ci/run_phase2_real_tracker_usb_host.sh"
echo "=== TRACKER TX HOST TESTI ==="
bash "$ROOT/tools/ci/run_sf5k3v_tracker_tx_host.sh"
echo "=== FAZ 2 REAL PILOT DERLEMESI ==="
if ! pio run -d "$ROOT/firmware" -e rak4630_phase2_tracker_real_pilot > "$LOG" 2>&1; then
  echo "FAIL: firmware derleme. Log: $LOG"
  tail -n 35 "$LOG"
  exit 1
fi
if fuser -s "$PORT"; then
  echo "DUR: Tracker portunu baska surec kullanmaya basladi."
  exit 2
fi
echo "=== SADECE YENI TRACKER / USB DFU UPLOAD ==="
if ! pio run -d "$ROOT/firmware" \
  -e rak4630_phase2_tracker_real_pilot -t upload \
  --upload-port "$PORT" >> "$LOG" 2>&1; then
  echo "FAIL: USB DFU upload. Yeniden upload yapma. Log: $LOG"
  tail -n 55 "$LOG"
  exit 1
fi
echo "=== YENIDEN BAGLANMA / KAYIT BUTUNLUGU ==="
inspect post
echo "RESULT: PHASE2_TRACKER_REAL_PILOT_UPLOAD_PASS"
