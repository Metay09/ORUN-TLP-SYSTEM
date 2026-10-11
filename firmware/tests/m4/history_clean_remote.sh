#!/bin/sh
# Clears a History partition that the production image refuses to use
# (APP STORAGE? history_ready=no, for example retained v2/v3 lab pages:
# "STORAGE v3 development reset required"), over USB, without unplugging.
#
#   1. flash REF (production) and read APP STORAGE?; if History is ready,
#      stop here: nothing is erased;
#   2. flash the TEST-ONLY M4P3 helper (rak4630_m4p3_history_v4_qual), print
#      every History page, send CLEAN: it erases ONLY 0x0ED000..0x0F3FFF and
#      read-verifies the region as 0xFF;
#   3. flash REF again: the blank History starts a fresh v4 stream with a new
#      incarnation (APP STORAGE? history_ready=yes).
#
# Only History is touched: config, geofence, security and BLE bonds stay.
# Each upload resets the device; it is not power-cycled (M4P3.md 5: the new
# stream becomes authoritative only through its committed first page).
#
# Usage, from the repository root, with the device's serial logger stopped:
#   sh firmware/tests/m4/history_clean_remote.sh <DEVICE_ID_HEX>
#
# It checks out and flashes REF, so the working tree must be clean. It ends on
# branch "main" with the device running REF. Device lines: ~/history_clean.log.
set -eu

# The checkout below replaces this file; keep running from a private copy.
if [ "${ORUN_HISTORY_CLEAN_COPY:-}" != 1 ]; then
  copy=$(mktemp)
  cp "$0" "$copy"
  ORUN_HISTORY_CLEAN_COPY=1 exec sh "$copy" "$@"
fi

ID=${1:?usage: sh $0 <DEVICE_ID_HEX>}
PORT="/dev/serial/by-id/usb-RAKwireless_WisCore_RAK4631_Board_${ID}-if00"
REF=${REF:-origin/feat/power-services-status}
LOG="$HOME/history_clean.log"
READER=""

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

finish() {
  stop_listen
  git checkout --quiet main
  echo
  echo "$1"
  exit "$2"
}

trap 'stop_listen' EXIT

# flash <env> <description>
flash() {
  echo "== flashing $2"
  stop_listen
  pio run -d firmware -e "$1" -t upload --upload-port "$PORT" \
    > "$LOG.build" 2>&1 || finish "FAIL build/upload of $2 (see $LOG.build)" 1
  sleep 3
  MARK=$(wc -l < "$LOG")
  listen
  sleep 15  # boot and store recovery
}

MARK=0
ask() {
  MARK=$(wc -l < "$LOG")
  printf '%s\n' "$1" > "$PORT"
  sleep 3
}

# since_mark <regex>: device lines since the last flash/ask that match.
since_mark() {
  tail -n +"$((MARK + 1))" "$LOG" | tr -d '\r' | grep -E "$1" || true
}

git checkout --quiet --detach "$REF"

flash rak4630 "$REF (production)"
since_mark '^STORAGE' | sed 's/^/     /'
ask "APP STORAGE?"
storage=$(since_mark '^APP STORAGE ')
echo "     ${storage:-no APP STORAGE reply}"
case "$storage" in
  *history_ready=yes*)
    finish "RESULT: History is ready; nothing erased (device left on $REF)" 0 ;;
  *history_ready=no*) ;;
  *) finish "FAIL no APP STORAGE reply; nothing erased (log: $LOG)" 1 ;;
esac

flash rak4630_m4p3_history_v4_qual "History clean helper (TEST-ONLY)"
ask "STATUS"
pages=$(since_mark '^M4P3 HISTORY (PAGE|STATUS)')
[ -n "$pages" ] || finish "FAIL helper did not report; nothing erased, device left on the helper (log: $LOG)" 1
printf '%s\n' "$pages" | sed 's/^/     /'
ask "CLEAN"
sleep 5
clean=$(since_mark '^M4P3 HISTORY CLEAN' | tail -n 1)
echo "     $clean"
case "$clean" in
  *"CLEAN PASS"*) echo "PASS History erased and verified" ;;
  *) finish "FAIL clean did not pass; device left on the helper, flash REF again before use (log: $LOG)" 1 ;;
esac

flash rak4630 "$REF (production)"
since_mark '^STORAGE' | sed 's/^/     /'
ask "APP STORAGE?"
storage=$(since_mark '^APP STORAGE ')
echo "     ${storage:-no APP STORAGE reply}"
case "$storage" in
  *"history_ready=yes"*"corrupt=0 "*)
    finish "RESULT: PASS History ready again, fresh stream (device left on $REF)" 0 ;;
  *) finish "RESULT: FAIL History still not ready (device on $REF; log: $LOG)" 1 ;;
esac
