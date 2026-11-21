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
  if [[ -d "$NEW" ]]; then
    [[ -f "$NEW/tools/orun_sunucu.py" ]] ||
      fail "Eksik sürüm dizini: $NEW"
    return 0
  fi
  local tmp
  tmp=$(mktemp -d "$RELEASES/.sunucu-hazir.XXXXXXXX")
  # Başarısız hazırlık hedef sürüm klasörüne dokunmaz.
  if ! (set -e
        git -C "$REPO" archive "$TARGET" backend tools | tar -x -C "$tmp"
        cd "$tmp"
        python3 -m unittest -q \
          backend.tests.test_observation_latest \
          backend.tests.test_orun_yedek
        python3 -m py_compile tools/orun_sunucu.py \
          backend/orun_backend_bench.py backend/observation_read_page.py
       ); then
    rm -rf -- "$tmp"
    fail "Yeni sürümün host testleri başarısız; servisler değiştirilmedi."
  fi
  mv -- "$tmp" "$NEW"
}

# Sağlık kontrolü: sürüm yolunu doğrula, gözlem sayısını koru,
# köprü yetkisini ve hesap yetki kapısını sorgula; token'ı argv'ye yazma.
check_sunucu() {
  local expected="${1:-$NEW}" minimum="${2:-0}"
  RECORDS=$(python3 - "$SUNUCU" "$expected" "$minimum" <<'PY'
import json
import subprocess
import sys
import urllib.error
import urllib.request
from pathlib import Path

unit, expected, minimum = sys.argv[1], sys.argv[2], int(sys.argv[3])
pid = subprocess.check_output(
    ["systemctl", "--user", "show", unit, "-p", "MainPID", "--value"],
    text=True).strip()
if not pid.isdigit() or int(pid) <= 0:
    raise SystemExit("SUNUCU_PID_YOK")
argv = [a.decode() for a in Path(f"/proc/{pid}/cmdline").read_bytes().split(b"\\0") if a]
if expected + "/tools/orun_sunucu.py" not in argv:
    raise SystemExit("SUNUCU_KODU_FARKLI")
if "--bench-test-key" in argv:
    raise SystemExit("BENCH_KEY_ETKIN")
def get(name):
    return argv[argv.index(name) + 1]
opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
token = Path(get("--bridge-token-file")).read_text().strip()
req = urllib.request.Request(
    "http://" + get("--bridge-listen") + "/v1/health",
    headers={"Authorization": "Bearer " + token})
with opener.open(req, timeout=5) as r:
    obj = json.load(r)
count = obj.get("records")
if obj.get("ok") is not True or type(count) is not int or count < minimum:
    raise SystemExit("KAYIT_AZALDI_VEYA_SAGLIK_YOK")
try:
    opener.open("http://" + get("--listen") + "/api/v1/account/me", timeout=5)
except urllib.error.HTTPError as e:
    if e.code != 401:
        raise SystemExit("HESAP_YETKI_KONTROLU_BASARISIZ")
else:
    raise SystemExit("HESAP_KIMLIKSIZ_ACIK")
print(count)
PY
  ) || return 1
  return 0
}

wait_sunucu() {
  local expected="${1:-$NEW}" minimum="${2:-0}" i
  for i in $(seq 1 20); do
    check_sunucu "$expected" "$minimum" 2>/dev/null && return 0
    sleep 1
  done
  return 1
}

backup_live() {
  local dest
  # Önce güncel sunucunun çalıştığı ve kayıtlar okunabildiği kanıtlanır.
  check_sunucu "$CURRENT" 0 || fail "Çalışan sunucu sağlık kontrolünden geçmedi."
  BEFORE="$RECORDS"
  dest=$(mktemp -d "$HOME/ORUN-GUNCELLEME-YEDEK.XXXXXXXX")
  chmod 700 "$dest"
  python3 "$CURRENT/tools/orun_yedek.py" al --hedef "$dest" --sakla 30 ||
    fail "Yedek alınamadı; servis değiştirilmedi."
  python3 "$CURRENT/tools/orun_yedek.py" prova --hedef "$dest" ||
    fail "Geri dönüş provası başarısız; servis değiştirilmedi."
  echo "YEDEK_KORUNDU=$dest"
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

# Sunucu yalnız değişmişse; eski sürümün kayıt ve yetki kontrolleri önce geçer.
if [[ "$UPDATE_SERVER" == 1 ]]; then
  [[ -n "$CURRENT" && -d "$CURRENT" && -f "$UNIT_FILE" ]] ||
    fail "Tek sunucu kurulumu bulunamadı; otomatik servis göçü yapılmaz."
  systemctl --user is-active --quiet "$SUNUCU" ||
    fail "Eski sunucu çalışmıyor; hiçbir şey değiştirilmedi."
  say "Önce mevcut sunucuyu yedekle ve yeni kodu doğrula"
  backup_live
  release_dir

  UNIT_BACKUP="$UNIT_FILE.bak-$STAMP"
  cp -p "$UNIT_FILE" "$UNIT_BACKUP"

  # Hata, kesinti veya yarıda bırakmada eski unit'i geri yükle.
  ROLLBACK_PENDING=1
  restore_server() {
    local rc=$?
    trap - EXIT
    if [[ "$ROLLBACK_PENDING" == 1 ]]; then
      echo "DUR: Sunucu sürümü geri alınıyor ($CURRENT)." >&2
      cp -p "$UNIT_BACKUP" "$UNIT_FILE" || true
      systemctl --user daemon-reload || true
      systemctl --user restart "$SUNUCU" || true
      if ! wait_sunucu "$CURRENT" "$BEFORE"; then
        echo "KRITIK: Eski sunucu da doğrulanamadı. Servis günlüklerini kontrol et." >&2
      fi
    fi
    exit "$rc"
  }
  trap restore_server EXIT

  python3 - "$UNIT_FILE" "$CURRENT" "$NEW" <<'PY'
import os
import stat
import sys
import tempfile
from pathlib import Path

file, old, new = sys.argv[1:]
path = Path(file)
src = path.read_text()
if src.count(old + "/tools/orun_sunucu.py") != 1:
    raise SystemExit("DUR: Beklenmeyen servis ExecStart yolu")
fixed = src.replace(old + "/tools/orun_sunucu.py",
                    new + "/tools/orun_sunucu.py")
fd, temp = tempfile.mkstemp(prefix=".orun-unit.", dir=path.parent)
try:
    os.fchmod(fd, stat.S_IMODE(path.stat().st_mode))
    with os.fdopen(fd, "w") as f:
        f.write(fixed)
        f.flush()
        os.fsync(f.fileno())
    os.replace(temp, path)
finally:
    if os.path.exists(temp):
        os.unlink(temp)
PY

  systemctl --user daemon-reload
  systemctl --user restart "$SUNUCU"
  if ! wait_sunucu "$NEW" "$BEFORE"; then
    fail "Yeni sunucu sağlık kontrolünden geçmedi; geri dönülecek."
  fi
  ROLLBACK_PENDING=0
  trap - EXIT
  echo "SUNUCU_GUNCELLENDI=$SHA KAYIT_ONCE=$BEFORE KAYIT_SONRA=$RECORDS"
else
  echo "SUNUCU_KORUNDU: sürüm veya seçili bileşen değişmedi."
fi

# PC Edge ayrı bileşendir; sunucu-only modu ASLA bu servisi restart etmez.
if [[ "$UPDATE_EDGE" == 1 ]]; then
  systemctl --user is-active --quiet "$PC_EDGE" ||
    fail "PC Edge çalışmıyor; otomatik Edge yükseltmesi reddedildi."
  release_dir
  DROPIN_DIR="$UNIT_DIR/$PC_EDGE.d"
  DROPIN="$DROPIN_DIR/90-orun-guncelle.conf"
  NEXT="$DROPIN.new-$$"
  say "Yalnız PC Edge için yeni çalışma yolu hazırla"
  python3 "$NEW/tools/orun_pc_gecis.py" pc-edge-dropin "$NEW" "$NEXT" ||
    fail "PC Edge başlatma ayarı üretilemedi."
  if [[ -f "$DROPIN" ]] && cmp -s "$NEXT" "$DROPIN"; then
    rm -f "$NEXT"
    echo "PC_EDGE_KORUNDU: mevcut ayar zaten aynı."
  else
    had_dropin=0
    [[ -f "$DROPIN" ]] && had_dropin=1
    [[ "$had_dropin" == 0 ]] || cp -p "$DROPIN" "$DROPIN.bak-$STAMP"
    mv "$NEXT" "$DROPIN"
    systemctl --user daemon-reload
    systemctl --user restart "$PC_EDGE" || true
    edge_ok=0
    for i in $(seq 1 10); do
      if systemctl --user is-active --quiet "$PC_EDGE"; then
        edge_ok=1
        break
      fi
      sleep 1
    done
    if [[ "$edge_ok" != 1 ]]; then
      echo "DUR: Edge başlamadı; önceki tanımına dönülüyor." >&2
      if [[ "$had_dropin" == 1 ]]; then
        cp -p "$DROPIN.bak-$STAMP" "$DROPIN"
      else
        rm -f "$DROPIN"
      fi
      systemctl --user daemon-reload
      systemctl --user restart "$PC_EDGE" || true
      fail "PC Edge güncellenemedi; eski ayara dönüldü."
    fi
    echo "PC_EDGE_GUNCELLENDI=$SHA"
  fi
else
  echo "PC_EDGE_KORUNDU: yeniden başlatılmadı."
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
