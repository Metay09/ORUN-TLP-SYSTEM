#!/bin/sh
# Physical check of the local USB geofence writer on one RAK4631 over USB.
# No power cut, no unplugging.
#
#   - flash REF, read the stored geofence and tracking interval;
#   - CLEAR: durable first, runtime clear, effective interval back to base B;
#   - REPLACE with a small square far from here (valid geometry), runtime
#     configured; an invalid set changes nothing;
#   - reboot: the stored square is recovered; CLEAR again and leave it clear.
#
# Usage, from the repository root, with the device's serial logger stopped:
#   sh firmware/tests/m6/geofence_usb_remote_check.sh <DEVICE_ID_HEX>
#
# It checks out and flashes REF, so the working tree must be clean. It ends on
# branch "main" with the device running REF and no geofence.
# Every device line it reads is kept in ~/geofence_usb_check.log.
set -eu

if [ "${ORUN_FENCE_CHECK_COPY:-}" != 1 ]; then
  copy=$(mktemp)
  cp "$0" "$copy"
  ORUN_FENCE_CHECK_COPY=1 exec sh "$copy" "$@"
fi

ID=${1:?usage: sh $0 <DEVICE_ID_HEX>}
PORT="/dev/serial/by-id/usb-RAKwireless_WisCore_RAK4631_Board_${ID}-if00"
REF=${REF:-origin/feat/usb-geofence-writer}
LOG="$HOME/geofence_usb_check.log"
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
  sleep 15
}

MARK=0
ask() {
  MARK=$(wc -l < "$LOG")
  printf '%s\n' "$1" > "$PORT"
  sleep 3
}

expect() {
  if tail -n +"$((MARK + 1))" "$LOG" | tr -d '\r' | grep -Eq "$1"; then
    echo "PASS $2"
  else
    echo "FAIL $2"
    FAILURES=$((FAILURES + 1))
  fi
  tail -n +"$((MARK + 1))" "$LOG" | tr -d '\r' |
    grep -E '^(APP |GEOFENCE )' | sed 's/^/     /'
}

# A ~100 m square in the Mediterranean, far from any test field.
square() {
  ask "APP FENCE BEGIN"
  ask "APP FENCE V 35.0000 30.0000"
  ask "APP FENCE V 35.0000 30.0010"
  ask "APP FENCE V 35.0010 30.0010"
  ask "APP FENCE V 35.0010 30.0000"
}

flash "$REF"
ask "APP GEOFENCE?"
expect 'APP GEOFENCE id=' "geofence status readable after the upgrade"
ask "APP TRACKING?"
expect 'APP TRACKING id=' "tracking status readable (effective_s before the clear)"

ask "APP FENCE CLEAR"
expect 'APP FENCE id=[0-9]+ code=(APPLIED|UNCHANGED) state=CLEAR' "clear saved"
ask "APP TRACKING?"
expect 'applied_s=([0-9]+) effective_s=\1 ' "interval back to base B after clear"

square
expect 'APP FENCE STAGING areas=1 vertices=4' "square staged"
ask "APP FENCE COMMIT"
expect 'GEOFENCE runtime configured areas=1 vertices=4 token=VALID' "runtime took the new square"
expect 'APP FENCE id=[0-9]+ code=APPLIED state=CONFIGURED areas=1 vertices=4 token=VALID' "square saved"

ask "APP FENCE BEGIN"
ask "APP FENCE V 35.0000 30.0000"
ask "APP FENCE V 35.0010 30.0010"
ask "APP FENCE COMMIT"
expect 'code=INVALID state=CONFIGURED areas=1 vertices=4' "two-point area refused, square kept"

flash "$REF"
ask "APP GEOFENCE?"
expect 'APP GEOFENCE id=.* areas=1 vertices=4 runtime=configured' "square recovered after reboot"
ask "HEALTH?"
expect 'HEALTH ' "loop health after reboot"
ask "APP FENCE CLEAR"
expect 'APP FENCE id=[0-9]+ code=APPLIED state=CLEAR areas=0 vertices=0 token=VALID' "cleared again"
ask "APP TRACKING?"
expect 'applied_s=([0-9]+) effective_s=\1 ' "interval at base B at the end"

stop_listen
git checkout --quiet main
echo
if [ "$FAILURES" -eq 0 ]; then
  echo "RESULT: ALL PASS (device left on $REF, no geofence)"
else
  echo "RESULT: $FAILURES FAIL (device left on $REF; log: $LOG)"
fi
