#!/usr/bin/env bash
# ORUN: PC'deki sunucuyu bu repo sürümüne geçirir, telefon APK'sını üretir
# ve telefona indirmek için kısa süre sunar.
#
#   bash tools/orun_guncelle.sh            # sunucu + APK + indirme
#   bash tools/orun_guncelle.sh --no-apk   # yalnız sunucu
#
# İlk çalıştırmada üç eski servisi (orun-backend, orun-shared-sync,
# orun-observation-readonly) tek "orun-sunucu" servisine çevirir: aynı
# veritabanı, aynı kimlik kayıtları, aynı hesap klasörü, aynı adresler.
# Sonraki çalıştırmalarda yalnız orun-sunucu'yu yeni sürüme geçirir.
# DOKUNMADIĞI: orun-pc-edge, veritabanı içeriği, anahtar dosyaları.
# Bir kontrol başarısız olursa eski hale kendisi döner.
set -euo pipefail

OLD_UNITS=(orun-observation-readonly.service orun-shared-sync.service orun-backend.service)
SUNUCU=orun-sunucu.service
UNIT_DIR="$HOME/.config/systemd/user"
SERVE_PORT=${ORUN_APK_PORT:-8099}

REPO=$(cd "$(dirname "$0")/.." && pwd)
SHA=$(git -C "$REPO" rev-parse --short=7 HEAD)
RELEASES="$HOME/ORUN-RELEASES"
NEW="$RELEASES/sunucu-$SHA"
STAMP=$(date +%Y%m%d-%H%M%S)
BUILD_APK=1
[[ "${1:-}" == "--no-apk" ]] && BUILD_APK=0

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
    cp -p "$UNIT_DIR/$SUNUCU" "$UNIT_DIR/$SUNUCU.bak-$STAMP"
    sed -i "s#$CURRENT#$NEW#g" "$UNIT_DIR/$SUNUCU"
    systemctl --user daemon-reload
    systemctl --user restart "$SUNUCU" || true
    sleep 3
    if ! check_sunucu; then
      cp -p "$UNIT_DIR/$SUNUCU.bak-$STAMP" "$UNIT_DIR/$SUNUCU"
      systemctl --user daemon-reload
      systemctl --user restart "$SUNUCU" || true
      fail "Yeni sürüm kontrolü geçemedi; eski sürüme dönüldü ($CURRENT). Bu çıktıyı Claude'a gönder."
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
  sleep 3
  check_sunucu || geri_don
  # Başarılı: eskiler açılışta başlamasın (dosyaları duruyor, geri dönülebilir).
  systemctl --user disable "${OLD_UNITS[@]}" 2>/dev/null || true
  systemctl --user enable "$SUNUCU" >/dev/null 2>&1 || true
  echo "Tamam: tek sunucu çalışıyor, $RECORDS kayıt. PC Edge aynı adrese yüklemeye devam ediyor."
  echo "Eskiye dönmek gerekirse: systemctl --user disable --now $SUNUCU && systemctl --user enable --now ${OLD_UNITS[*]}"
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
OUT="$APK_DIR/ORUN-$SHA.apk"
cp -f "$APK" "$OUT"
echo "APK: $OUT"

# --- Telefona indirme (yalnız Tailscale ağı içinden) ---
IP=$(tailscale ip -4 2>/dev/null | head -1 || true)
if [[ -z "$IP" ]]; then
  echo "Tailscale adresi bulunamadı; APK'yı kendin aktar: $OUT"
  exit 0
fi
say "Telefondan indir: http://$IP:$SERVE_PORT/ORUN-$SHA.apk"
echo "İndirme bitince bu pencerede Ctrl+C'ye bas. (Yalnız Tailscale ağına açık; en fazla 15 dakika.)"
timeout 900 python3 -m http.server "$SERVE_PORT" --bind "$IP" --directory "$APK_DIR" || true
echo "İndirme sunucusu kapandı."
