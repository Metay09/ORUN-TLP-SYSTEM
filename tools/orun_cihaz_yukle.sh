#!/usr/bin/env bash
# Y1: USB'deki ORUN cihazına (tasma ya da gateway) tek komutla yazılım yükler.
#
#   bash tools/orun_cihaz_yukle.sh                 # tek cihazsa onu, birden çoksa numarayla seçtirir
#   bash tools/orun_cihaz_yukle.sh --port /dev/serial/by-id/usb-RAKwireless_...-if00
#   bash tools/orun_cihaz_yukle.sh --evet          # onay sormadan
#
# Sırası: cihazı tanı (tasma mı gateway mi) → durumunu oku → doğru yazılımı
# derle → USB'den yükle → aynı cihaz mı, anahtarı ve kayıtları yerinde mi diye
# yeniden oku. Silme komutu YOK; bootloader'a dokunulmaz. Gateway PC Edge'e
# bağlıysa PC Edge yükleme boyunca durdurulur ve sonunda kendiliğinden başlar.
set -euo pipefail

REPO=$(cd "$(dirname "$0")/.." && pwd)
FW="$REPO/firmware"
LOG="$HOME/orun-cihaz-yukle.log"
PC_EDGE=orun-pc-edge.service
PORT=""
YES=0
while [[ $# -gt 0 ]]; do
  case "$1" in
    --port) PORT="$2"; shift 2 ;;
    --evet) YES=1; shift ;;
    *) echo "Bilinmeyen seçenek: $1"; exit 2 ;;
  esac
done

say()  { printf '\n== %s\n' "$*"; }
fail() { printf '\nDUR: %s\n' "$*" >&2; exit 1; }

# Hangi cihaz türüne hangi yazılım (Y2'de ürün yazılımına geçer).
env_for() {
  case "$1" in
    tracker) echo rak4630_sf5l0_tracker_bench ;;
    gateway) echo rak4630_sf5l0_gateway_bench ;;
    *) return 1 ;;
  esac
}

PIO=$(command -v pio || true)
[[ -n "$PIO" ]] || PIO="$HOME/.platformio/penv/bin/pio"
[[ -x "$PIO" ]] || fail "PlatformIO (pio) bulunamadı."
python3 -c "import serial" 2>/dev/null || fail "pyserial yok: python3 -m pip install --user pyserial"

# --- 1. Cihazı bul ---
if [[ -z "$PORT" ]]; then
  mapfile -t ports < <(ls "${ORUN_BYID_DIR:-/dev/serial/by-id}"/usb-RAKwireless_WisCore_RAK4631_Board_*-if00 2>/dev/null || true)
  if [[ ${#ports[@]} -eq 0 ]]; then fail "USB'de ORUN (RAK4631) cihazı yok. Kabloyu tak, tekrar dene."; fi
  if [[ ${#ports[@]} -gt 1 ]]; then
    # Birden çok cihaz: numaralı liste, numarayla seçilir. PC Edge'in tuttuğu
    # port gateway'dir; bunu da yanında yazar.
    edge_pid=$(systemctl --user show -p MainPID --value "$PC_EDGE" 2>/dev/null || echo 0)
    echo "Birden çok cihaz takılı:"
    for i in "${!ports[@]}"; do
      p="${ports[$i]}"; id=${p##*_Board_}; id=${id%-if00}
      holder=$(fuser "$p" 2>/dev/null | tr -s ' ' '\n' | grep -E '^[0-9]+$' | head -1 || true)
      note=""
      [[ -n "$holder" && "$holder" == "$edge_pid" ]] && note=" (PC Edge'e bağlı gateway)"
      printf '  %d) %s%s\n' "$((i + 1))" "$id" "$note"
    done
    [[ $YES -eq 0 ]] || fail "--evet ile birden çok cihaz seçilemez; --port ile söyle."
    read -r -p "Hangisine yüklensin? [1-${#ports[@]}] " pick
    [[ "$pick" =~ ^[0-9]+$ ]] && (( pick >= 1 && pick <= ${#ports[@]} )) || { echo "Vazgeçildi; cihaza dokunulmadı."; exit 0; }
    PORT="${ports[$((pick - 1))]}"
  else
    PORT="${ports[0]}"
  fi
fi
[[ -e "$PORT" ]] || fail "$PORT yok."

# --- 2. Port boş mu (gateway ise PC Edge tutuyor olabilir) ---
STOPPED_PC_EDGE=0
restart_pc_edge() {
  if [[ $STOPPED_PC_EDGE -eq 1 ]]; then
    echo "PC Edge yeniden başlatılıyor…"
    systemctl --user start "$PC_EDGE" || echo "UYARI: PC Edge başlatılamadı: systemctl --user start $PC_EDGE"
    STOPPED_PC_EDGE=0
  fi
}
trap restart_pc_edge EXIT
if fuser -s "$PORT" 2>/dev/null; then
  holder=$(fuser "$PORT" 2>/dev/null | tr -s ' ' '\n' | grep -E '^[0-9]+$' | head -1 || true)
  edge_pid=$(systemctl --user show -p MainPID --value "$PC_EDGE" 2>/dev/null || echo 0)
  if [[ -n "$holder" && "$holder" == "$edge_pid" ]]; then
    echo "Bu cihazı PC Edge kullanıyor; yükleme boyunca durdurulacak, sonunda kendiliğinden başlar."
    systemctl --user stop "$PC_EDGE"
    STOPPED_PC_EDGE=1
    sleep 2
  else
    fail "Portu başka bir program kullanıyor (pid ${holder:-?}). O programı kapatıp tekrar dene."
  fi
fi

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"; restart_pc_edge' EXIT

# --- 3. Ne bu cihaz, durumu ne ---
say "Cihaz okunuyor"
python3 "$REPO/tools/orun_cihaz_kontrol.py" oku --port "$PORT" --out "$TMP/once.json" \
  || fail "Cihaz okunamadı. Kabloyu çıkarıp tak, tekrar dene."
KIND=$(python3 -c "import json,sys; print(json.load(open(sys.argv[1]))['kind'])" "$TMP/once.json")
ENV=$(env_for "$KIND") || fail "Bilinmeyen cihaz türü: $KIND"
if [[ "$KIND" == gateway && ! -r "$HOME/orun-issuer.pub" ]]; then
  fail "Gateway yazılımı için ~/orun-issuer.pub gerekli (sunucunun imza açık anahtarı)."
fi
SHA=$(git -C "$REPO" rev-parse --short=7 HEAD)
echo "Yüklenecek: $ENV · sürüm $SHA (kayıtlar ve anahtarlar korunur)"
if [[ $YES -eq 0 ]]; then
  read -r -p "Devam edilsin mi? [e/H] " ans
  [[ "$ans" == e || "$ans" == E ]] || { echo "Vazgeçildi; cihaza dokunulmadı."; exit 0; }
fi

# --- 4. Derle ---
say "Derleniyor ($ENV)"
{ echo "=== $(date -Is) $ENV $SHA port=$PORT"; } >> "$LOG"
if ! "$PIO" run -d "$FW" -e "$ENV" >> "$LOG" 2>&1; then
  tail -n 30 "$LOG"
  fail "Derleme başarısız; cihaza hiçbir şey yüklenmedi. Ayrıntı: $LOG"
fi
grep -E "^(RAM|Flash):" "$LOG" | tail -2 || true

# --- 5. Yükle ---
say "Yükleniyor (USB)"
if ! "$PIO" run -d "$FW" -e "$ENV" -t upload --upload-port "$PORT" >> "$LOG" 2>&1; then
  tail -n 30 "$LOG"
  fail "Yükleme başarısız. Cihazı çıkarıp takmadan TEKRAR DENEME; bu çıktıyı Claude'a gönder. Ayrıntı: $LOG"
fi

# --- 6. Aynı cihaz mı, anahtar ve kayıtlar yerinde mi ---
say "Yeniden okunuyor"
sleep 3
python3 "$REPO/tools/orun_cihaz_kontrol.py" oku --port "$PORT" --out "$TMP/sonra.json" --deneme 20 \
  || fail "Yüklemeden sonra cihaz okunamadı. Bu çıktıyı Claude'a gönder."
if python3 "$REPO/tools/orun_cihaz_kontrol.py" karsilastir "$TMP/once.json" "$TMP/sonra.json"; then
  echo
  echo "TAMAM: yazılım yüklendi, cihaz aynı, anahtarı ve kayıtları yerinde."
else
  fail "Yükleme bitti ama yukarıdaki SORUN satırları var. Cihazı kullanmadan önce bu çıktıyı Claude'a gönder."
fi
