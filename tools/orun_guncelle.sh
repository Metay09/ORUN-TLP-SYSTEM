#!/usr/bin/env bash
# ORUN mevcut güvenli güncelleyici. Varsayılan: yalnız değişen PC bileşenleri.
# bash tools/orun_guncelle.sh          # sunucu / PC Edge: sadece kod değiştiyse
# bash tools/orun_guncelle.sh --check  # yalnız planı göster, servis değişmez
# bash tools/orun_guncelle.sh --server # yalnız sunucu; eski --no-apk eşdeğeri
# bash tools/orun_guncelle.sh --edge   # yalnız PC Edge
# bash tools/orun_guncelle.sh --apk    # debug APK sadece açık istekle
# bash tools/orun_guncelle.sh --all    # bütün PC bileşenleri; firmware hariç
# Firmware flash veya telefona APK kurulumunu ASLA kendiliğinden yapmaz.
set -euo pipefail
umask 077

OLD_UNITS=(orun-observation-readonly.service orun-shared-sync.service orun-backend.service)
SUNUCU=orun-sunucu.service
PC_EDGE=orun-pc-edge.service
UNIT_DIR="$HOME/.config/systemd/user"
SERVE_PORT=${ORUN_APK_PORT:-8099}
REPO=$(cd "$(dirname "$0")/.." && pwd)
RELEASES="$HOME/ORUN-RELEASES"
STAMP=$(date +%Y%m%d-%H%M%S)

MODE="${1:---auto}"
[[ $# -le 1 ]] || { echo "HATA: Tek seçenek kullanın." >&2; exit 2; }
case "$MODE" in
  --auto|--check|--server|--no-apk|--edge|--apk|--all) ;;
  -h|--help)
    sed -n '2,9p' "$0"
    exit 0 ;;
  *) echo "HATA: Bilinmeyen seçenek: $MODE" >&2; exit 2 ;;
esac

mkdir -p "$RELEASES"
# İki aynı güncelleme aynı anda çalışamaz.
exec 9>"$RELEASES/.orun-guncelle.lock"
flock -n 9 || { echo "HATA: Başka bir güncelleme çalışıyor." >&2; exit 2; }

# Yalnız GitHub'daki main'i hedefle; yerel worktree'ye ve çalışan servislere
# fetch sırasında dokunma. Beklenmeyen branch/duruma güvenme.
[[ "$(git -C "$REPO" branch --show-current)" == "main" ]] ||
  { echo "DUR: Önce ana main çalışma klasörünü kullanın." >&2; exit 2; }
[[ -z "$(git -C "$REPO" status --porcelain --untracked-files=no)" ]] ||
  { echo "DUR: Yerel takipli kod değişiklikleri var." >&2; exit 2; }
git -C "$REPO" -c http.version=HTTP/1.1 fetch --no-tags origin main
TARGET=$(git -C "$REPO" rev-parse FETCH_HEAD)
git -C "$REPO" merge-base --is-ancestor HEAD "$TARGET" ||
  { echo "DUR: Yerel main, hedefle fast-forward uyumlu değil." >&2; exit 2; }
SHA=$(git -C "$REPO" rev-parse --short=7 "$TARGET")
NEW="$RELEASES/sunucu-$SHA"

UNIT_FILE="$UNIT_DIR/$SUNUCU"
CURRENT=""
if [[ -f "$UNIT_FILE" ]]; then
  CURRENT=$(grep -oE "$RELEASES/sunucu-[0-9a-f]{7,40}" "$UNIT_FILE" | head -1 || true)
fi

# Kod farkını git üzerinde kontrol et: aynı kod için servis yeniden başlatma.
has_change() {
  local before="$1"; shift
  local code=0
  git -C "$REPO" diff --quiet "$before" "$TARGET" -- "$@" || code=$?
  case $code in
    0) return 1 ;;
    1) return 0 ;;
    *) echo "DUR: Git fark kontrolü başarısız." >&2; exit 2 ;;
  esac
}
UPDATE_SERVER=0
UPDATE_EDGE=0
BUILD_APK=0

if [[ -n "$CURRENT" && -d "$CURRENT" ]]; then
  OLD_SHA=$(git -C "$REPO" rev-parse --verify "${CURRENT##*/sunucu-}^{commit}" 2>/dev/null) ||
    { echo "DUR: Kurulu sunucu commit'i bilinmiyor." >&2; exit 2; }
  if [[ "$CURRENT" != "$NEW" ]] && has_change "$OLD_SHA" \
       backend tools/orun_sunucu.py tools/orun_shared_sync_loopback.py; then
    UPDATE_SERVER=1
  fi
  if [[ "$CURRENT" != "$NEW" ]] && has_change "$OLD_SHA" \
       tools/orun_pc_edge.py tools/orun_edge_bench.py tools/orun_edge_accept.py; then
    UPDATE_EDGE=1
  fi
else
  [[ "$MODE" == --server || "$MODE" == --no-apk || "$MODE" == --all ]] ||
    { echo "DUR: Tek sunucu henüz kurulu değil; otomatik geçiş yapılmaz." >&2; exit 2; }
  UPDATE_SERVER=1
fi

case "$MODE" in
  --server|--no-apk) UPDATE_EDGE=0 ;;
  --edge) UPDATE_SERVER=0; UPDATE_EDGE=1 ;;
  --apk) UPDATE_SERVER=0; UPDATE_EDGE=0; BUILD_APK=1 ;;
  --all) UPDATE_SERVER=1; UPDATE_EDGE=1; BUILD_APK=1 ;;
esac
[[ "$CURRENT" == "$NEW" ]] && UPDATE_SERVER=0
[[ "$MODE" != --auto ]] || echo "Otomatik seçim: değişmiş PC modülleri"
echo "HEDEF=$TARGET"
echo "SUNUCU=$UPDATE_SERVER PC_EDGE=$UPDATE_EDGE APK=$BUILD_APK"
echo "Firmware: ASLA otomatik yüklenmez."
if [[ "$MODE" == --check ]]; then exit 0; fi
if [[ "$UPDATE_SERVER" == 0 && "$UPDATE_EDGE" == 0 && "$BUILD_APK" == 0 ]]; then
  echo "DEGISIKLIK YOK: Çalışan servisler korunuyor."
  exit 0
fi

say()  { printf '\n== %s\n' "$*"; }
fail() { printf '\nHATA: %s\n' "$*" >&2; exit 1; }

release_dir() {
  if [[ ! -d "$NEW" ]]; then
    mkdir -p "$NEW.tmp"
    git -C "$REPO" archive HEAD backend tools | tar -x -C "$NEW.tmp"
    mv "$NEW.tmp" "$NEW"
  fi
}

# Çalışan orun-sunucu'nun köprü adresi, hesap portu ve köprü anahtar dosyası.
check_sunucu() {
  local info bridge port token_file code
  info=$(python3 "$REPO/tools/orun_pc_gecis.py" describe) || return 1
  bridge=$(sed -n 's/^BRIDGE=//p' <<<"$info")
  port=$(sed -n 's/^ACCOUNT_PORT=//p' <<<"$info")
  token_file=$(sed -n 's/^TOKEN_FILE=//p' <<<"$info")
  systemctl --user is-active --quiet "$SUNUCU" || return 1
  [[ -r "$token_file" ]] || return 1
  code=$(curl -s -o /tmp/orun-health.json -w '%{http_code}' \
         -H "Authorization: Bearer $(tr -d '\r\n' < "$token_file")" \
         "http://$bridge/v1/health" || true)
  [[ "$code" == "200" ]] || { rm -f /tmp/orun-health.json; return 1; }
  RECORDS=$(grep -o '"records":[0-9]*' /tmp/orun-health.json | cut -d: -f2)
  rm -f /tmp/orun-health.json
  # Hesap adresi ayakta mı (girişsiz istek 401 dönmeli).
  code=$(curl -s -o /dev/null -w '%{http_code}' "http://127.0.0.1:$port/api/v1/account/me" || true)
  [[ "$code" == "401" ]]
}

# Sunucu 20 saniyeye kadar ayağa kalkabilir (kayıt defteri açılıyor).
wait_sunucu() {
  local i
  for i in $(seq 1 20); do
    check_sunucu && return 0
    sleep 1
  done
  return 1
}

# Eski üç servisten biri yeniden çalışıyorsa sunucunun adresini kapar
# (2026-11-01: orun-shared-sync 127.0.0.1:18781'i tuttu). İşlerini
# orun-sunucu yapıyor; durdurulur ve açılışta başlamaması sağlanır.
eskileri_durdur() {
  local u
  for u in "${OLD_UNITS[@]}"; do
    if systemctl --user is-active --quiet "$u"; then
      echo "Eski $u çalışıyordu; durduruldu (işini orun-sunucu yapıyor)."
      systemctl --user stop "$u" || true
      systemctl --user disable "$u" >/dev/null 2>&1 || true
    fi
  done
}

mkdir -p "$UNIT_DIR"

if systemctl --user cat "$SUNUCU" >/dev/null 2>&1; then
  # --- Güncelleme: orun-sunucu zaten var ---
  say "orun-sunucu yeni sürüme geçiriliyor"
  CURRENT=$(grep -o -E "$RELEASES/sunucu-[0-9a-f]+" "$UNIT_DIR/$SUNUCU" | head -1 || true)
  [[ -n "$CURRENT" ]] || fail "$UNIT_DIR/$SUNUCU içinde sürüm yolu yok; hiçbir şey değişmedi."
  if [[ "$CURRENT" == "$NEW" ]]; then
    echo "Sunucu zaten bu sürümde."
  else
    release_dir
    eskileri_durdur
    cp -p "$UNIT_DIR/$SUNUCU" "$UNIT_DIR/$SUNUCU.bak-$STAMP"
    sed -i "s#$CURRENT#$NEW#g" "$UNIT_DIR/$SUNUCU"
    systemctl --user daemon-reload
    systemctl --user restart "$SUNUCU" || true
    if ! wait_sunucu; then
      echo "--- yeni sürümün son kayıtları ---" >&2
      journalctl --user -u "$SUNUCU" -n 15 --no-pager >&2 || true
      cp -p "$UNIT_DIR/$SUNUCU.bak-$STAMP" "$UNIT_DIR/$SUNUCU"
      systemctl --user daemon-reload
      systemctl --user restart "$SUNUCU" || true
      if wait_sunucu; then
        fail "Yeni sürüm kontrolü geçemedi; eski sürüme dönüldü ve eski sürüm çalışıyor ($CURRENT). Bu çıktıyı Claude'a gönder."
      fi
      fail "Yeni sürüm açılmadı, eski sürüm de açılmadı! Sunucu kapalı. Bu çıktıyı hemen Claude'a gönder."
    fi
    echo "Tamam: sunucu $SHA sürümünde, $RECORDS kayıt."
  fi
else
  # --- İlk geçiş: üç servisten tek servise ---
  say "Üç eski servis tek sunucuya çevriliyor"
  for u in "${OLD_UNITS[@]}"; do
    systemctl --user cat "$u" >/dev/null 2>&1 || fail "$u bulunamadı; hiçbir şey değişmedi."
  done
  release_dir
  python3 "$REPO/tools/orun_pc_gecis.py" write-unit "$NEW" "$UNIT_DIR/$SUNUCU" \
    || fail "Eski servis ayarları okunamadı; hiçbir şey değişmedi."
  echo "Yazıldı: $UNIT_DIR/$SUNUCU"

  geri_don() {
    printf '\nKontrol başarısız; eski üç servise dönülüyor…\n' >&2
    systemctl --user stop "$SUNUCU" 2>/dev/null || true
    mv -f "$UNIT_DIR/$SUNUCU" "$UNIT_DIR/$SUNUCU.basarisiz-$STAMP"
    systemctl --user daemon-reload
    systemctl --user start orun-backend.service orun-observation-readonly.service orun-shared-sync.service || true
    fail "Eski servislerle devam ediliyor. Bu çıktıyı Claude'a gönder."
  }

  systemctl --user daemon-reload
  systemctl --user stop "${OLD_UNITS[@]}" || geri_don
  systemctl --user start "$SUNUCU" || geri_don
  wait_sunucu || geri_don
  # Başarılı: eskiler açılışta başlamasın (dosyaları duruyor, geri dönülebilir).
  systemctl --user disable "${OLD_UNITS[@]}" 2>/dev/null || true
  systemctl --user enable "$SUNUCU" >/dev/null 2>&1 || true
  echo "Tamam: tek sunucu çalışıyor, $RECORDS kayıt. PC Edge aynı adrese yüklemeye devam ediyor."
  echo "Eskiye dönmek gerekirse: systemctl --user disable --now $SUNUCU && systemctl --user enable --now ${OLD_UNITS[*]}"
fi

# --- PC Edge (gateway köprüsü) da aynı sürüme geçer ---
# Başarısız olursa yalnız PC Edge eski haline döner; sunucu adımı etkilenmez.
PC_EDGE=orun-pc-edge.service
DROPIN_DIR="$UNIT_DIR/$PC_EDGE.d"
DROPIN="$DROPIN_DIR/90-orun-guncelle.conf"
if systemctl --user cat "$PC_EDGE" >/dev/null 2>&1; then
  say "PC Edge yeni sürüme geçiriliyor"
  [[ -f "$DROPIN" ]] && cp -p "$DROPIN" "$DROPIN.bak-$STAMP"
  if python3 "$REPO/tools/orun_pc_gecis.py" pc-edge-dropin "$NEW" "$DROPIN.new"; then
    mv -f "$DROPIN.new" "$DROPIN"
    systemctl --user daemon-reload
    systemctl --user restart "$PC_EDGE" || true
    sleep 4
    if systemctl --user is-active --quiet "$PC_EDGE"; then
      echo "Tamam: PC Edge $SHA sürümünde (gateway iznini kendiliğinden günceller)."
    else
      echo "PC Edge yeni sürümle başlamadı; eski haline dönülüyor." >&2
      if [[ -f "$DROPIN.bak-$STAMP" ]]; then cp -p "$DROPIN.bak-$STAMP" "$DROPIN"; else rm -f "$DROPIN"; fi
      systemctl --user daemon-reload
      systemctl --user restart "$PC_EDGE" || true
    fi
  else
    rm -f "$DROPIN.new"
    echo "PC Edge güncellenmedi (yukarıdaki nedenle); eskisi çalışmaya devam ediyor."
  fi
fi

# --- İnternetsiz harita paketi (yalnız hiç yoksa; yenilemek için orun_harita.sh) ---
MAPS="${XDG_DATA_HOME:-$HOME/.local/share}/orun/maps"
if ! compgen -G "$MAPS/aydin-mugla-*.pmtiles.json" >/dev/null; then
  say "İnternetsiz harita paketi hazırlanıyor (ilk kez)"
  bash "$REPO/tools/orun_harita.sh" \
    || echo "Harita paketi hazırlanamadı; sonra tekrar dene: bash tools/orun_harita.sh"
fi

[[ $BUILD_APK -eq 1 ]] || exit 0

# --- APK ---
say "Telefon uygulaması (APK) derleniyor"
cd "$REPO/edge/app"
npm ci --no-audit --no-fund
npm run android
(cd android && ./gradlew -q assembleDebug)
APK_DIR="$REPO/edge/app/android/app/build/outputs/apk/debug"
APK="$APK_DIR/app-debug.apk"
[[ -f "$APK" ]] || fail "APK oluşmadı."
# İndirme klasörü yalnız ORUN sürümlerini tutar (en yeni 5), sayfada en
# yeni üstte büyük, eskiler altta tarihleriyle (tools/orun_apk_sayfa.py).
APK_SITE="$RELEASES/apk"
python3 "$REPO/tools/orun_apk_sayfa.py" ekle "$APK_SITE" "$APK" "$SHA" \
  "$(git -C "$REPO" log -1 --format=%s)"
OUT="$APK_SITE/ORUN-$SHA.apk"
echo "APK: $OUT"

# --- Telefona indirme (yalnız Tailscale ağı içinden) ---
IP=$(tailscale ip -4 2>/dev/null | head -1 || true)
if [[ -z "$IP" ]]; then
  echo "Tailscale adresi bulunamadı; APK'yı kendin aktar: $OUT"
  exit 0
fi
for p in $(seq "$SERVE_PORT" $((SERVE_PORT + 9))); do
  if ! (exec 3<>"/dev/tcp/$IP/$p") 2>/dev/null; then SERVE_PORT=$p; break; fi
done
say "Telefondan aç: http://$IP:$SERVE_PORT/  (en yeni sürüm en üstte: ORUN $SHA)"
echo "İndirme bitince bu pencerede Ctrl+C'ye bas. (Yalnız Tailscale ağına açık; en fazla 15 dakika.)"
# --foreground: Ctrl+C sunucuya ulaşsın (yoksa timeout onu ayrı süreç grubunda tutar).
trap 'echo; echo "İndirme sunucusu kapandı."; exit 0' INT
timeout --foreground 900 python3 -m http.server "$SERVE_PORT" --bind "$IP" --directory "$APK_SITE" || true
echo "İndirme sunucusu kapandı."
