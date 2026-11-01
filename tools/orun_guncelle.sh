#!/usr/bin/env bash
# ORUN: PC'deki iki servisi bu repo sürümüne geçirir, telefon APK'sını üretir
# ve telefona indirmek için kısa süre sunar.
#
#   bash tools/orun_guncelle.sh            # servisler + APK + indirme
#   bash tools/orun_guncelle.sh --no-apk   # yalnız servisler
#
# Dokunduğu servisler: orun-observation-readonly, orun-shared-sync.
# DOKUNMADIĞI: orun-backend, orun-pc-edge, veritabanı, anahtar/token dosyaları.
# Bir kontrol başarısız olursa servis dosyalarını yedekten geri yükler ve
# eski sürümle yeniden başlatır.
set -euo pipefail

READONLY_UNIT=orun-observation-readonly.service
BRIDGE_UNIT=orun-shared-sync.service
READONLY_PORT=${ORUN_READONLY_PORT:-18782}
SERVE_PORT=${ORUN_APK_PORT:-8099}

REPO=$(cd "$(dirname "$0")/.." && pwd)
SHA=$(git -C "$REPO" rev-parse --short=7 HEAD)
RELEASES="$HOME/ORUN-RELEASES"
NEW="$RELEASES/observation-$SHA"
STAMP=$(date +%Y%m%d-%H%M%S)
BUILD_APK=1
[[ "${1:-}" == "--no-apk" ]] && BUILD_APK=0

say()  { printf '\n== %s\n' "$*"; }
fail() { printf '\nHATA: %s\n' "$*" >&2; exit 1; }

# --- 1. Şu an çalışan sürüm (servis dosyalarından okunur, tahmin edilmez) ---
say "Çalışan sürüm bulunuyor"
OLD=$(systemctl --user cat "$READONLY_UNIT" "$BRIDGE_UNIT" 2>/dev/null \
      | grep -o -E "(/[^ ]*|~|%h)/ORUN-RELEASES/observation-[0-9a-f]+" | sort -u || true)
[[ -n "$OLD" ]] || fail "Servis dosyalarında ORUN-RELEASES/observation-… yolu bulunamadı; hiçbir şey değişmedi."
[[ $(wc -l <<<"$OLD") -eq 1 ]] || fail "İki servis farklı sürümleri gösteriyor:
$OLD
Hiçbir şey değişmedi."
OLD_ABS=${OLD/#\~/$HOME}
OLD_ABS=${OLD_ABS/#%h/$HOME}
echo "Şu an: $OLD_ABS"
echo "Yeni:  $NEW"
if [[ "$OLD_ABS" == "$NEW" ]]; then
  echo "Servisler zaten bu sürümde."
else
  # --- 2. Yeni sürüm klasörü (yalnız backend/ ve tools/) ---
  say "Yeni sürüm klasörü hazırlanıyor"
  if [[ ! -d "$NEW" ]]; then
    mkdir -p "$NEW.tmp"
    git -C "$REPO" archive HEAD backend tools | tar -x -C "$NEW.tmp"
    mv "$NEW.tmp" "$NEW"
  fi

  # --- 3. Servis dosyalarını yedekle ve yeni yola çevir ---
  say "Servis dosyaları güncelleniyor (yedekli)"
  FILES=()
  for unit in "$READONLY_UNIT" "$BRIDGE_UNIT"; do
    for f in $(systemctl --user show -p FragmentPath -p DropInPaths --value "$unit"); do
      [[ -f "$f" ]] && grep -q -F "$OLD" "$f" && FILES+=("$f")
    done
  done
  [[ ${#FILES[@]} -gt 0 ]] || fail "Değiştirilecek servis dosyası bulunamadı; hiçbir şey değişmedi."
  for f in "${FILES[@]}"; do
    cp -p "$f" "$f.bak-$STAMP"
    sed -i "s#$OLD#$NEW#g" "$f"
    echo "  $f (yedek: $f.bak-$STAMP)"
  done

  rollback() {
    printf '\nKontrol başarısız; eski sürüme dönülüyor…\n' >&2
    for f in "${FILES[@]}"; do cp -p "$f.bak-$STAMP" "$f"; done
    systemctl --user daemon-reload
    systemctl --user restart "$READONLY_UNIT" "$BRIDGE_UNIT" || true
    fail "Eski sürüme dönüldü ($OLD_ABS). Bu çıktıyı Claude'a gönder."
  }

  # --- 4. Yeniden başlat ve kontrol et ---
  say "Servisler yeniden başlatılıyor"
  systemctl --user daemon-reload
  systemctl --user restart "$READONLY_UNIT" "$BRIDGE_UNIT" || rollback
  sleep 3
  for unit in "$READONLY_UNIT" "$BRIDGE_UNIT"; do
    systemctl --user is-active --quiet "$unit" || rollback
  done

  # Yeni uç gerçekten çalışıyor mu: token dosyası servis satırından okunur, ekrana basılmaz.
  TOKEN_FILE=$(systemctl --user cat "$READONLY_UNIT" | grep -o -E -- "--token-file[= ][^ ]+" | head -1 | sed -E 's/--token-file[= ]//')
  TOKEN_FILE=${TOKEN_FILE/#\~/$HOME}
  TOKEN_FILE=${TOKEN_FILE/#%h/$HOME}
  [[ -r "$TOKEN_FILE" ]] || rollback
  CODE=$(curl -s -o /tmp/orun-latest-check.json -w '%{http_code}' \
         -H "Authorization: Bearer $(tr -d '\r\n' < "$TOKEN_FILE")" \
         "http://127.0.0.1:$READONLY_PORT/v1/edge/records?latest=1" || true)
  if [[ "$CODE" != "200" ]] || ! grep -q '"latest": *true' /tmp/orun-latest-check.json; then
    rm -f /tmp/orun-latest-check.json; rollback
  fi
  N=$(grep -o '"device"' /tmp/orun-latest-check.json | wc -l)
  rm -f /tmp/orun-latest-check.json
  echo "Tamam: iki servis çalışıyor, sunucu $N tasmanın son konumunu veriyor."
  echo "Geri dönmek gerekirse: yedek dosyalar .bak-$STAMP, eski sürüm $OLD_ABS"
fi

[[ $BUILD_APK -eq 1 ]] || exit 0

# --- 5. APK ---
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

# --- 6. Telefona indirme (yalnız Tailscale ağı içinden) ---
IP=$(tailscale ip -4 2>/dev/null | head -1 || true)
if [[ -z "$IP" ]]; then
  echo "Tailscale adresi bulunamadı; APK'yı kendin aktar: $OUT"
  exit 0
fi
say "Telefondan indir: http://$IP:$SERVE_PORT/ORUN-$SHA.apk"
echo "İndirme bitince bu pencerede Ctrl+C'ye bas. (Yalnız Tailscale ağına açık; en fazla 15 dakika.)"
timeout 900 python3 -m http.server "$SERVE_PORT" --bind "$IP" --directory "$APK_DIR" || true
echo "İndirme sunucusu kapandı."
