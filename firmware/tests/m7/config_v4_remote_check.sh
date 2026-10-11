#!/bin/sh
# Physical check of ORUN_CONFIG_STORE_V4_SERVICE_INTENT.md section 9,
# items 2 and 5, on one development RAK4631 over USB. No power cut needed.
#
#   2. A device that holds a v2 record: boot v4 (no write), first save,
#      reboot, record recovered.
#   5. Previous firmware over the v4 record: defaults, changes refused, page
#      kept; v4 again: record recovered unchanged.
#
# Usage, from the repository root, with the device's serial logger stopped:
#   sh firmware/tests/m7/config_v4_remote_check.sh <DEVICE_ID_HEX>
#
# It checks out and flashes two refs (V4_REF, OLD_REF), so the working tree
# must be clean. It ends on OLD_REF's branch name "main" with the device
# running V4_REF. Every device line it reads is kept in ~/config_v4_check.log.
set -eu

# The checkouts below replace this file; keep running from a private copy.
if [ "${ORUN_V4_CHECK_COPY:-}" != 1 ]; then
  copy=$(mktemp)
  cp "$0" "$copy"
  ORUN_V4_CHECK_COPY=1 exec sh "$copy" "$@"
fi

ID=${1:?usage: sh $0 <DEVICE_ID_HEX>}
PORT="/dev/serial/by-id/usb-RAKwireless_WisCore_RAK4631_Board_${ID}-if00"
V4_REF=${V4_REF:-origin/feat/config-store-v4}
OLD_REF=${OLD_REF:-origin/main}
LOG="$HOME/config_v4_check.log"
READER=""
FAILURES=0

[ -e "$PORT" ] || { echo "No device at $PORT"; exit 1; }
if ! git diff --quiet || ! git diff --cached --quiet; then
  echo "Working tree has local changes; commit or stash first."
  exit 1
fi
git fetch --quiet origin
: > "$LOG"

listen() {
  for _ in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20; do
    [ -e "$PORT" ] && break
    sleep 1
  done
  stty -F "$PORT" 115200 raw -echo
  cat "$PORT" >> "$LOG" &
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
  echo "== flashing $1"
  stop_listen
  git checkout --quiet --detach "$1"
  pio run -d firmware -e rak4630 -t upload --upload-port "$PORT" \
    > "$LOG.build" 2>&1 || { echo "FAIL build/upload of $1 (see $LOG.build)"; exit 1; }
  sleep 3
  listen
  sleep 10  # boot, stores recovered, GNSS/role settled
}

MARK=0
ask() {
  MARK=$(wc -l < "$LOG")
  printf '%s\n' "$1" > "$PORT"
  sleep 3
}

# expect <regex> <description>: the reply to the last ask() must match.
expect() {
  if tail -n +"$((MARK + 1))" "$LOG" | tr -d '\r' | grep -Eq "$1"; then
    echo "PASS $2"
  else
    echo "FAIL $2"
    FAILURES=$((FAILURES + 1))
  fi
  tail -n +"$((MARK + 1))" "$LOG" | tr -d '\r' | grep -E '^APP ' | sed 's/^/     /'
}

flash "$V4_REF"
ask "APP CONFIG?"
expect 'code=OK .*source=stored' "v4 boot recovers the stored v2 record"
ask "APP STORAGE?"
expect 'config_ready=yes config_maintenance=no' "no maintenance after v4 boot"
ask "APP INTERVAL 300"
expect 'APP SET .*code=APPLIED tracking_interval_seconds=300 token=VALID' "first save under v4 applied"

flash "$V4_REF"
ask "APP CONFIG?"
expect 'source=stored tracking_interval_seconds=300' "v4 record recovered after reboot"

flash "$OLD_REF"
ask "APP CONFIG?"
expect 'source=default tracking_interval_seconds=180' "old firmware runs on defaults over a v4 record"
ask "APP STORAGE?"
expect 'config_maintenance=yes' "old firmware reports maintenance"
ask "APP INTERVAL 600"
expect 'APP SET .*code=MAINTENANCE' "old firmware refuses config changes"

flash "$V4_REF"
ask "APP CONFIG?"
expect 'source=stored tracking_interval_seconds=300' "v4 record intact after the downgrade"
ask "APP STORAGE?"
expect 'config_maintenance=no' "no maintenance after returning to v4"
ask "APP INTERVAL 180"
expect 'code=APPLIED tracking_interval_seconds=180' "interval restored to 180"

stop_listen
git checkout --quiet main
echo
if [ "$FAILURES" -eq 0 ]; then
  echo "RESULT: ALL PASS (device left on $V4_REF)"
else
  echo "RESULT: $FAILURES FAIL (device left on $V4_REF; log: $LOG)"
fi
