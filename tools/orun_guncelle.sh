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

SUNUCU=orun-sunucu.service
PC_EDGE=orun-pc-edge.service
UNIT_DIR="$HOME/.config/systemd/user"
REPO=$(cd "$(dirname "$0")/.." && pwd)
RELEASES="$HOME/ORUN-RELEASES"
STAMP=$(date +%Y%m%d-%H%M%S)

say()  { printf '\n== %s\n' "$*"; }
fail() { printf '\nHATA: %s\n' "$*" >&2; exit 1; }

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
ORIGIN=$(git -C "$REPO" remote get-url origin)
case "$ORIGIN" in
  https://github.com/Metay09/ORUN-TLP-SYSTEM|\
  https://github.com/Metay09/ORUN-TLP-SYSTEM.git|\
  git@github.com:Metay09/ORUN-TLP-SYSTEM.git|\
  ssh://git@github.com/Metay09/ORUN-TLP-SYSTEM.git) ;;
  *) fail "DUR: GitHub origin beklenen ORUN deposu değil." ;;
esac
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
  # PC Edge farklı bir release altında çalışabilir: sunucu SHA'sına
  # bakıp yanlışlıkla Edge restart kararı verme.
  EDGE_CURRENT=$(python3 - "$PC_EDGE" <<'PY'
import subprocess
import sys
from pathlib import Path
pid = subprocess.run(
    ["systemctl", "--user", "show", sys.argv[1], "-p", "MainPID", "--value"],
    capture_output=True, text=True, check=False).stdout.strip()
try:
    argv = [a.decode() for a in Path(f"/proc/{int(pid)}/cmdline").read_bytes().split(bytes((0,))) if a]
    files = [Path(a) for a in argv if a.endswith("/tools/orun_pc_edge.py")]
    if len(files) == 1:
        print(files[0].parents[1])
except (ValueError, OSError):
    pass
PY
  )
  if [[ -n "$EDGE_CURRENT" && -d "$EDGE_CURRENT" &&
        "$EDGE_CURRENT" == "$RELEASES"/sunucu-* ]]; then
    EDGE_REF="${EDGE_CURRENT##*/sunucu-}"
    EDGE_SHA=$(git -C "$REPO" rev-parse --verify "${EDGE_REF}^{commit}" 2>/dev/null) ||
      fail "Kurulu PC Edge commit'i bilinmiyor."
    if has_change "$EDGE_SHA" tools/orun_pc_edge.py \
         tools/orun_edge_bench.py tools/orun_edge_accept.py; then
      UPDATE_EDGE=1
    fi
  else
    echo "UYARI: PC Edge çalışma commit'i okunamadı; otomatik Edge geçişi atlanıyor."
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
argv = [a.decode() for a in Path(f"/proc/{pid}/cmdline").read_bytes().split(bytes((0,))) if a]
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
  LAST_BACKUP="$dest"
  echo "YEDEK_KORUNDU=$dest"
}

# Yalnız eksik olan ölçüm indeksleri; önce doğrulanmış yedeğin kopyasında
# dener. SQL işlemi tek transaction; OBS1/ACK/cursor satırları değişmez.
migrate_latest_indexes() {
  python3 - "$NEW" "$LAST_BACKUP" <<'PY'
import shutil
import sqlite3
import sys
import tempfile
from contextlib import closing
from pathlib import Path

release = Path(sys.argv[1]).resolve()
backup = Path(sys.argv[2]).resolve()
sys.path.insert(0, str(release))
from backend.observation_read_page import LATEST_INDEXES, _LOCATION, _REPORT, _STAMP

# Gerçek DB yolunu çalışan sunucu sürecinden doğrula.
import subprocess
pid = subprocess.check_output(
    ["systemctl", "--user", "show", "orun-sunucu.service",
     "-p", "MainPID", "--value"], text=True).strip()
args = [a.decode() for a in Path(f"/proc/{pid}/cmdline").read_bytes().split(bytes((0,))) if a]
dbpath = Path(args[args.index("--db") + 1]).resolve(strict=True)

expected = ("latest_location_utc", "latest_report_utc")
def present(db):
    return {r[0] for r in db.execute(
        "SELECT name FROM sqlite_master WHERE type='index'")}

with closing(sqlite3.connect(dbpath.as_uri() + "?mode=ro",
                             uri=True, timeout=2)) as ro:
    if all(name in present(ro) for name in expected):
        print("INDEKSLER_MEVCUT: SQLite yazımı yapılmadı.")
        raise SystemExit(0)

snapshots = [
    d / "kayit_defteri.sqlite"
    for d in backup.iterdir()
    if d.is_dir() and (d / "LISTE.json").is_file()
]
if len(snapshots) != 1 or not snapshots[0].is_file():
    raise SystemExit("DUR: Doğrulanmış defter yedeği bulunamadı.")

def apply(path):
    with closing(sqlite3.connect(path.as_uri() + "?mode=rw",
                                 uri=True, timeout=2)) as db:
        with db:
            db.execute("BEGIN IMMEDIATE")
            before = db.execute(
                "SELECT COUNT(*) FROM verified_observations"
            ).fetchone()[0]
            for ddl in LATEST_INDEXES:
                db.execute(ddl)
            for name, where in (
                ("latest_location_utc", _LOCATION),
                ("latest_report_utc", _REPORT)
            ):
                plan = db.execute(
                    f"EXPLAIN QUERY PLAN SELECT {_STAMP},rowid "
                    f"FROM verified_observations WHERE device_id=? "
                    f"AND {where} AND {_STAMP}>=? "
                    f"ORDER BY {_STAMP} DESC,rowid DESC LIMIT 1",
                    (bytes(8), bytes(4))
                ).fetchall()
                detail = " ".join(str(r[-1]) for r in plan)
                if name not in detail or "TEMP B-TREE" in detail:
                    raise RuntimeError("INDEKS_PLAN_HATALI: " + name)
            after = db.execute(
                "SELECT COUNT(*) FROM verified_observations"
            ).fetchone()[0]
            if before != after:
                raise RuntimeError("GOZLEM_SAYISI_DEGISTI")
    with closing(sqlite3.connect(path.as_uri() + "?mode=ro",
                                 uri=True, timeout=2)) as ro:
        if not set(expected).issubset(present(ro)):
            raise RuntimeError("INDEKS_DOGRULAMA_BASARISIZ")

with tempfile.TemporaryDirectory(dir=backup) as td:
    test = Path(td) / "test.sqlite"
    shutil.copy2(snapshots[0], test)
    apply(test)
    print("INDEKS_PROVA_PASS")

apply(dbpath)
print("INDEKS_CANLI_PASS")
PY
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
  migrate_latest_indexes

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
    EDGE_ROLLBACK=1
    restore_edge() {
      local rc=$?
      trap - EXIT
      if [[ "$EDGE_ROLLBACK" == 1 ]]; then
        echo "DUR: PC Edge eski servis ayarına dönüyor." >&2
        if [[ "$had_dropin" == 1 ]]; then
          cp -p "$DROPIN.bak-$STAMP" "$DROPIN" || true
        else
          rm -f "$DROPIN"
        fi
        systemctl --user daemon-reload || true
        systemctl --user restart "$PC_EDGE" || true
        systemctl --user is-active "$PC_EDGE" || true
      fi
      exit "$rc"
    }
    trap restore_edge EXIT
    mv "$NEXT" "$DROPIN"
    systemctl --user daemon-reload
    systemctl --user restart "$PC_EDGE"
    edge_ok=0
    for i in $(seq 1 10); do
      if python3 - "$PC_EDGE" "$NEW" <<'PY'
import subprocess
import sys
from pathlib import Path
unit, release = sys.argv[1:]
p = subprocess.run(
    ["systemctl", "--user", "show", unit, "-p", "MainPID", "--value"],
    capture_output=True, text=True).stdout.strip()
if not p.isdigit() or int(p) <= 0:
    raise SystemExit(1)
args = [v.decode() for v in Path(f"/proc/{p}/cmdline").read_bytes().split(bytes((0,))) if v]
if release + "/tools/orun_pc_edge.py" not in args:
    raise SystemExit(1)
PY
      then
        sleep 1
        if systemctl --user is-active --quiet "$PC_EDGE"; then
          edge_ok=1
          break
        fi
      fi
      sleep 1
    done
    [[ "$edge_ok" == 1 ]] || fail "PC Edge yeni sürümde sağlıklı değil."
    EDGE_ROLLBACK=0
    trap - EXIT
    echo "PC_EDGE_GUNCELLENDI=$SHA"
  fi
else
  echo "PC_EDGE_KORUNDU: yeniden başlatılmadı."
fi

# Android mevcut kurulu sürüme dokunmaz. Debug APK yalnız açık --apk/--all
# isteğiyle GitHub hedef SHA'dan derlenir; telefon otomatik güncellenmez.
[[ "$BUILD_APK" == 1 ]] || exit 0

APK_SITE="$RELEASES/apk"
OUT="$APK_SITE/ORUN-$SHA.apk"
if [[ -f "$OUT" ]]; then
  echo "APK_ZATEN_VAR=$OUT (yeniden derlenmedi)"
  exit 0
fi

say "İsteğe bağlı debug APK derleniyor (telefon kurulumu yapılmaz)"
APP_TMP=$(mktemp -d "$RELEASES/.orun-android.XXXXXXXX")
trap 'rm -rf -- "$APP_TMP"' EXIT
git -C "$REPO" archive "$TARGET" edge/app | tar -x -C "$APP_TMP"
(
  cd "$APP_TMP/edge/app"
  npm ci --no-audit --no-fund
  npm run android
  cd android
  ./gradlew -q assembleDebug
)
APK="$APP_TMP/edge/app/android/app/build/outputs/apk/debug/app-debug.apk"
[[ -f "$APK" ]] || fail "APK üretilemedi"
python3 "$REPO/tools/orun_apk_sayfa.py" ekle "$APK_SITE" "$APK" "$SHA" \
  "$(git -C "$REPO" log -1 --format=%s "$TARGET")"
[[ -f "$OUT" ]] || fail "APK yayımlama klasöründe bulunamadı"
echo "APK_DEBUG_HAZIR=$OUT"
echo "Bu dosya release-imzalı değildir; kurulum kullanıcı onayı gerektirir."
