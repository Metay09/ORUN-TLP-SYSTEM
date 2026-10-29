#!/usr/bin/env bash
# ORUN S4D — independent runtime rehearsal beside EXISTING LIVE PC Edge.
#
# Real local HTTP process + real Step3C local PC enrollment + real PC mirror
# CLI, then restart and resume. All state/credentials are created under a
# uniquely generated, temporary 0700 directory and removed on exit.
# NEVER issues systemctl stop/start, never changes current observation DB,
# tracker, gateway, phone APK or ~/ORUN-PC-EDGE-VERIFY.
set -euo pipefail
umask 077

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"
command -v python3 >/dev/null
command -v systemctl >/dev/null

services=(orun-backend.service orun-pc-edge.service)
declare -a original_pids
for service in "${services[@]}"; do
  active="$(systemctl --user is-active "$service" || true)"
  pid="$(systemctl --user show "$service" -p MainPID --value)"
  if [[ "$active" != "active" || ! "$pid" =~ ^[1-9][0-9]*$ ]]; then
    echo "DUR: $service aktif degil; canli duruma dokunulmadi."
    exit 1
  fi
  original_pids+=("$pid")
  echo "KORUNAN_CANLI_SERVIS=$service PID=$pid"
done

PORT="$(python3 - <<'PY'
import socket
with socket.socket() as sock:
    sock.bind(("127.0.0.1", 0))
    print(sock.getsockname()[1])
PY
)"

WORK="$(mktemp -d -t orun-shared-s4d.XXXXXXXX)"
server_pid=""
cleanup() {
  if [[ -n "$server_pid" ]]; then
    kill "$server_pid" 2>/dev/null || true
    wait "$server_pid" 2>/dev/null || true
  fi
  if [[ -n "$WORK" && -d "$WORK" && "$(basename "$WORK")" == orun-shared-s4d.* ]]; then
    rm -rf -- "$WORK"
  fi
}
trap cleanup EXIT

mkdir -m 700 "$WORK/backend" "$WORK/secrets"

launch() {
  local mode="$1" i
  server_pid=""
  if [[ "$mode" == "reuse" ]]; then
    python3 -u tools/orun_shared_sync_loopback.py \
      --state-dir "$WORK/backend" --bind 127.0.0.1 \
      --port "$PORT" --reuse-state >"$WORK/server.log" 2>&1 &
  else
    python3 -u tools/orun_shared_sync_loopback.py \
      --state-dir "$WORK/backend" --bind 127.0.0.1 \
      --port "$PORT" >"$WORK/server.log" 2>&1 &
  fi
  server_pid="$!"
  for ((i=0; i<80; i++)); do
    if grep -Fxq "ORUN_SHARED_BIND=127.0.0.1:$PORT" "$WORK/server.log"; then
      echo "IZOLE_BACKEND_HAZIR=127.0.0.1:$PORT MODE=$mode"
      return 0
    fi
    if ! kill -0 "$server_pid" 2>/dev/null; then
      echo "DUR: IZOLE_BACKEND_START_FAILED"
      sed -n '1,12p' "$WORK/server.log" | sed -E 's/(Bearer|token|secret)[^ ]*/[REDACTED]/Ig'
      return 1
    fi
    sleep 0.1
  done
  echo "DUR: IZOLE_BACKEND_TIMEOUT"
  return 1
}

echo "=== AYRI ORTAK VERI BACKEND / PC EDGE CANLI PROVA ==="
launch first

python3 tools/orun_shared_local_pc_enroll.py \
  --state-dir "$WORK/backend" \
  --site integration-demo --actor owner \
  --installation pc-shadow-test \
  --token-out "$WORK/secrets/pc-shadow.token" \
  --authorize-local-pc

echo "=== YETKILI PC EDGE ILK CEKME ==="
python3 tools/orun_shared_pc_mirror.py \
  --db "$WORK/pc-shadow.sqlite" \
  --site integration-demo --installation pc-shadow-test \
  --url "http://127.0.0.1:$PORT" \
  --token-file "$WORK/secrets/pc-shadow.token" --once

kill "$server_pid"
wait "$server_pid" 2>/dev/null || true
server_pid=""
echo "=== BACKEND YENIDEN BASLAT VE DURUMU KORU ==="
launch reuse

python3 tools/orun_shared_pc_mirror.py \
  --db "$WORK/pc-shadow.sqlite" \
  --site integration-demo --installation pc-shadow-test \
  --url "http://127.0.0.1:$PORT" \
  --token-file "$WORK/secrets/pc-shadow.token" --once

echo "=== CANLI SERVISLER DEGISMEDI MI ==="
for i in "${!services[@]}"; do
  active="$(systemctl --user is-active "${services[$i]}" || true)"
  after="$(systemctl --user show "${services[$i]}" -p MainPID --value)"
  if [[ "$active" != "active" || "$after" != "${original_pids[$i]}" ]]; then
    echo "UYARI: ${services[$i]} PID veya durum degismis; analiz gerekli."
    exit 1
  fi
  echo "KORUNDU=${services[$i]} PID=$after"
done
echo "ORUN_S4D_REAL_PC_SHADOW_PASS"
echo "NOT: Bu izole prova ortak kaynak sayisi 0; uretilen test tokeni ve DB kapanista silinir."
echo "NOT: Canli sistemde ortak mera senkronizasyonu HENUZ ETKIN DEGIL."
