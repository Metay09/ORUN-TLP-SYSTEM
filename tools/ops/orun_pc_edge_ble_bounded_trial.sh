#!/usr/bin/env bash
# ORUN: bounded real PC Edge BLE-to-Backend data-path probe.
# Uses one existing user service, unchanged backend/SQLite/token; preserves
# original USB unit/10-bench-release.conf and restores them automatically.
# Never sends GATT ACCEPT or erases custody; BLE mode is receive/upload only.
set -euo pipefail
umask 077

REPO="$HOME/ORUN-PC-EDGE-VERIFY"
WORK="$HOME/ORUN-BLUEZ-566-HOTFIX"
SOURCE="$WORK/ble-pc-edge-transport-trial-src"
UNIT="orun-pc-edge.service"
PORT="/dev/serial/by-id/usb-RAKwireless_WisCore_RAK4631_Board_09A462BD4B275BA5-if00"
MAC="C5:DD:01:85:C9:4B"
URL="http://100.114.36.9:8090"
EDGE="pc-simsek"
TOKEN="$HOME/.config/orun/pc-edge.token"
USERDIR="$HOME/.config/systemd/user"
DROP_DIR="$USERDIR/orun-pc-edge.service.d"
BASE="$USERDIR/orun-pc-edge.service"
BENCH="$DROP_DIR/10-bench-release.conf"
DROP="$DROP_DIR/30-orun-ble-readonly-trial.conf"
WATCH="orun-edge-ble-trial-restore-$(date +%s)-$$"
CHANGED=0
WATCH_ARMED=0
STARTED=""

# Keep BLE trial short and bounded; watchdog extends past observation window.
TRIAL_SECONDS=90

check_usb_restored() {
  systemctl --user is-active --quiet "$UNIT" &&
    systemctl --user show "$UNIT" -p ExecStart | grep -Fq -- "--mode usb " &&
    systemctl --user show "$UNIT" -p ExecStart | grep -Fq -- "$PORT"
}

restore_usb() {
  rc=$?
  trap - EXIT HUP INT TERM
  if [ "$CHANGED" = 1 ]; then
    echo "=== RESTORE ORIGINAL USB PC EDGE ==="
    rm -f -- "$DROP"
    systemctl --user daemon-reload || rc=1
    systemctl --user restart "$UNIT" || rc=1
    if check_usb_restored; then
      echo "USB RESTORE: PASS"
      if [ "$WATCH_ARMED" = 1 ]; then
        systemctl --user stop "$WATCH.timer" || true
      fi
    else
      echo "USB RESTORE: NEEDS ATTENTION - rollback timer remains armed" >&2
      rc=1
    fi
  fi
  echo "=== FINAL SERVICE STATES ==="
  systemctl is-active bluetooth.service || true
  systemctl --user is-active orun-pc-edge.service || true
  systemctl --user is-active orun-backend.service || true
  exit "$rc"
}

echo "=== FAIL-CLOSED USB/BLE/DB PREFLIGHT ==="
test -f "$BASE" && test -f "$BENCH"
test ! -e "$DROP"
test ! -e "$DROP_DIR/20-ble-transport.conf"
test -e "$PORT"
test -r "$TOKEN"
test "$(systemctl is-active bluetooth.service)" = active
test "$(systemctl --user is-active orun-pc-edge.service)" = active
test "$(systemctl --user is-active orun-backend.service)" = active
systemctl show bluetooth.service -p ExecStart |
  grep -Fq '/usr/local/libexec/orun-bluez/bluetoothd-5.66-gattfix'
test "$(sha256sum /usr/local/libexec/orun-bluez/bluetoothd-5.66-gattfix | awk '{print $1}')" = \
  14412d0a913463f516d310c52f06a86ce0f5c6cc2748796733d596ac9bea4e05
check_usb_restored

python3 - "$BASE" "$BENCH" "$PORT" "$URL" "$EDGE" "$TOKEN" <<'PY'
from pathlib import Path
import shlex, sys
base, bench, port, url, edge, token = map(Path, sys.argv[1:3]) if False else (
    Path(sys.argv[1]), Path(sys.argv[2]), *sys.argv[3:]
)
def cmd(p):
    lines=[x[len("ExecStart="):] for x in p.read_text().splitlines()
           if x.startswith("ExecStart=") and x!="ExecStart="]
    assert len(lines)==1, "Unexpected ExecStart count"
    return shlex.split(lines[0])
a=cmd(base); b=cmd(bench)
assert a[:3] == b[:3], "Service executable changed"
assert Path(a[2]).name == "orun_pc_edge.py", "Unknown service executable"
expected=["--mode","usb","--backend-url",url,"--edge-id",edge,
          "--token-file",token,"--port",port]
assert a[3:]==expected, "Unknown base service options; abort"
assert b==a+["--bench-release-gateway","09A462BD4B275BA5"], (
    "Unexpected bench drop-in; abort")
print("EXISTING USB SERVICE CONFIG: MATCH")
PY

# A git archive snapshot guarantees BLE trial imports the same coherent source.
# The existing user's running repo and installed USB ExecStart are untouched.
test ! -e "$SOURCE"
git -C "$REPO" fetch origin feat/pc-edge-sealed-usb-ble-backend-spool
REV="$(git -C "$REPO" rev-parse FETCH_HEAD)"
mkdir -p "$SOURCE/tools"
for path in tools/orun_pc_edge.py tools/orun_edge_bench.py; do
  git -C "$REPO" show "$REV:$path" > "$SOURCE/$path"
done
python3 -m py_compile "$SOURCE/tools/orun_pc_edge.py" "$SOURCE/tools/orun_edge_bench.py"
python3 - <<'PY'
import bleak
print("BLEAK: PASS")
PY
python3 - "$SOURCE" "$TOKEN" <<'PY'
from pathlib import Path
import sys
sys.path.insert(0, str(Path(sys.argv[1]) / "tools"))
from orun_pc_edge import checked_backend, read_token
assert checked_backend("http://100.114.36.9:8090")
assert read_token(Path(sys.argv[2]))
print("STAGED BLE BACKEND/TOKEN PREFLIGHT: PASS")
PY

# Preserve canonical DB. Only read the baseline; sqlite opens read-only.
echo "=== SEALED RECORD BASELINE ==="
python3 - <<'PY'
from pathlib import Path
import sqlite3
db=Path.home()/".local/share/orun/pc-edge-inbox.sqlite"
if not db.is_file():
    raise SystemExit("Missing existing PC Edge SQLite inbox")
uri=db.resolve().as_uri()+"?mode=ro"
with sqlite3.connect(uri, uri=True) as con:
    print("SEALED_RECORDS:", con.execute("SELECT count(*) FROM sealed_records").fetchone()[0])
    print("BACKEND_STORED_OR_DUPLICATE:", con.execute(
        "SELECT count(*) FROM sealed_records WHERE backend_state IN ('STORED','DUPLICATE')").fetchone()[0])
PY

# Existing timer could inadvertently restart USB during this BLE trial.
if systemctl --user list-units --type=timer --state=active --plain --no-legend \
  'orun-edge-ble-trial-restore-*.timer' |
  grep -Fq 'orun-edge-ble-trial-restore-'; then
  echo "Another BLE restore watchdog is active; abort" >&2
  exit 1
fi

trap restore_usb EXIT
trap 'exit 130' INT
trap 'exit 143' TERM HUP

echo "=== ARM INDEPENDENT 180-SECOND USB RESTORE ==="
systemd-run --user --unit="$WATCH" --on-active=180s \
  /usr/bin/bash -c \
  'rm -f "$HOME/.config/systemd/user/orun-pc-edge.service.d/30-orun-ble-readonly-trial.conf"; systemctl --user daemon-reload; systemctl --user restart orun-pc-edge.service'
WATCH_ARMED=1
systemctl --user is-active --quiet "$WATCH.timer"

echo "=== APPLY ONE-SERVICE BLE READ/UPLOAD-ONLY OVERRIDE ==="
# No --bench-release-gateway parameter. No release or ACK is possible on BLE.
cat > "$DROP" <<CFG
[Service]
ExecStart=
ExecStart=/usr/bin/python3 -u $SOURCE/tools/orun_pc_edge.py --mode ble --backend-url $URL --edge-id $EDGE --token-file $TOKEN --ble-address $MAC
CFG
chmod 0600 "$DROP"
CHANGED=1
systemctl --user daemon-reload
systemctl --user restart "$UNIT"
systemctl --user is-active --quiet "$UNIT"
systemctl --user show "$UNIT" -p ExecStart | grep -Fq -- '--mode ble '
if systemctl --user show "$UNIT" -p ExecStart | grep -Fq -- '--bench-release-gateway'; then
  echo "FAIL: custody-release flag unexpectedly enabled!" >&2
  exit 1
fi

echo "=== BLE PC EDGE TRIAL WINDOW: 90 SECONDS ==="
sleep "$TRIAL_SECONDS"
echo "=== BLE PC EDGE / BACKEND EVIDENCE ==="
journalctl --user -u "$UNIT" --since "2 minutes ago" --no-pager -o cat |
  grep -E '^\[ble\]|^\[backend\]|^\[pc-edge\]' | tail -n 70 || true

echo "=== POST-TRIAL SEALED RECORD COUNTS ==="
python3 - <<'PY'
from pathlib import Path
import sqlite3
db=Path.home()/".local/share/orun/pc-edge-inbox.sqlite"
with sqlite3.connect(db.resolve().as_uri()+"?mode=ro",uri=True) as con:
    print("SEALED_RECORDS:",con.execute("SELECT count(*) FROM sealed_records").fetchone()[0])
    print("BACKEND_STORED_OR_DUPLICATE:",con.execute(
        "SELECT count(*) FROM sealed_records WHERE backend_state IN ('STORED','DUPLICATE')").fetchone()[0])
PY

echo "=== RETURN TO USB IN EXIT HANDLER ==="
