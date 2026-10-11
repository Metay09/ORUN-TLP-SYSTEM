#!/bin/sh
# SF5D4: does a normal firmware update keep all 32 pages of the candidate
# product store [0x0C5000, 0x0E5000)? Gate D1 evidence on a DEVELOPMENT
# RAK4631 only (owner accepts loss of test data on it). Over USB, no
# unplugging:
#
#   1. flash rak4630_sf5d_full_seed; "SF5D FULL SEED CONFIRM" writes a 16-byte
#      marker at the start of every fully erased candidate page (never erases,
#      leaves pages with other data alone) and prints the 32 page CRCs;
#   2. flash the normal production image rak4630 (the update under test) and
#      let it run for a minute;
#   3. flash rak4630_sf5d_full_verify (read-only) and read the 32 page CRCs;
#   4. compare; flash production back.
#
# Usage, from the repository root on the branch that has the probes, with
# that device's serial logger stopped:
#   sh firmware/tests/sf5/sf5d_full_retention_remote.sh <DEVICE_ID_HEX>
# Device lines go to ~/sf5d_full.log.
set -eu

if [ "${ORUN_SF5D_FULL_COPY:-}" != 1 ]; then
  copy=$(mktemp)
  cp "$0" "$copy"
  ORUN_SF5D_FULL_COPY=1 exec sh "$copy" "$@"
fi

ID=${1:?usage: sh $0 <DEVICE_ID_HEX>}
PORT="/dev/serial/by-id/usb-RAKwireless_WisCore_RAK4631_Board_${ID}-if00"
LOG="$HOME/sf5d_full.log"
READER=""
MARK=0

[ -e "$PORT" ] || { echo "Cihaz yok: $PORT"; exit 1; }
if pgrep -f "orunlog.sh $ID" > /dev/null 2>&1; then
  echo "Bu cihazin logger'i calisiyor. Once durdur:  pkill -f \"orunlog.sh $ID\""
  exit 1
fi
: > "$LOG"

listen() {
  for _ in $(seq 1 30); do
    [ -e "$PORT" ] && break
    sleep 1
  done
  for _ in $(seq 1 10); do
    stty -F "$PORT" 115200 raw -echo 2>/dev/null && break
    sleep 0.5
  done
  cat "$PORT" >> "$LOG" 2>/dev/null &
  READER=$!
}

stop_listen() {
  if [ -n "$READER" ]; then
    kill "$READER" 2>/dev/null || true
    wait "$READER" 2>/dev/null || true
    READER=""
  fi
}

trap 'stop_listen' EXIT

flash() {
  echo "== yukleniyor: $2"
  stop_listen
  pio run -d firmware -e "$1" -t upload --upload-port "$PORT" \
    > "$LOG.build" 2>&1 || { echo "FAIL build/upload of $2 (see $LOG.build)"; exit 1; }
  sleep 3
  MARK=$(wc -l < "$LOG")
  listen
}

since_mark() { tail -n +"$((MARK + 1))" "$LOG" | tr -d '\r'; }

wait_for() {
  for _ in $(seq 1 "$2"); do
    since_mark | grep -q -E "$1" && return 0
    sleep 1
  done
  return 1
}

# The 32 "address crc" pairs of the last full status after the current mark.
pages() {
  since_mark | awk '
    /^SF5D FULL PAGE / { a = $4; c = $6; sub("address=", "", a); sub("crc32=", "", c)
                         line[++n] = a " " c }
    /^SF5D FULL STATUS / { out = ""; for (i = n - 31; i <= n; i++) out = out line[i] "\n";
                           status = $0 }
    END { printf "%s", out }'
}

last_status() { since_mark | grep '^SF5D FULL STATUS ' | tail -n 1; }

restore() {
  flash rak4630 "production"
  sleep 10
  stop_listen
}

# 1. Seed.
flash rak4630_sf5d_full_seed "SF5D full seed (TEST-ONLY)"
wait_for '^SF5D FULL STATUS ' 30 || { echo "FAIL: seed image did not report"; restore; exit 1; }
echo "     once:  $(last_status)"
MARK=$(wc -l < "$LOG")
printf 'SF5D FULL SEED CONFIRM\n' > "$PORT"
wait_for '^SF5D FULL STATUS ' 30 || { echo "FAIL: no seed result"; restore; exit 1; }
seed=$(since_mark | grep -E '^SF5D FULL SEED (PASS|FAIL)' | tail -n 1)
echo "     $seed"
case "$seed" in
  "SF5D FULL SEED PASS "*) ;;
  *) echo "FAIL: seed"; restore; exit 1 ;;
esac
before_status=$(last_status)
echo "     sonra: $before_status"
case "$before_status" in
  *" erased=0 "*) ;;
  *) echo "FAIL: some candidate page is still blank"; restore; exit 1 ;;
esac
pages > "$LOG.before"

# 2. The update under test: the normal production image.
flash rak4630 "production (guncelleme testi)"
sleep 60

# 3. Read back.
flash rak4630_sf5d_full_verify "SF5D full verify (TEST-ONLY, read-only)"
wait_for '^SF5D FULL STATUS ' 30 || { echo "FAIL: verify image did not report"; restore; exit 1; }
after_status=$(last_status)
echo "     guncellemeden sonra: $after_status"
pages > "$LOG.after"

# 4. Production back, then the verdict.
restore

if [ "$(wc -l < "$LOG.before")" -eq 32 ] && cmp -s "$LOG.before" "$LOG.after"; then
  echo; echo "RESULT: PASS (32/32 sayfa ayni; cihaz production'da)"
else
  echo; echo "RESULT: FAIL (farkli sayfalar asagida; log: $LOG)"
  diff "$LOG.before" "$LOG.after" || true
  exit 1
fi
echo "Logger'i tekrar baslat:  sh ~/orunlog.sh $ID >> ~/tasma.log &"
