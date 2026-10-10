#!/bin/sh
# Physical check of ORUN_CONFIG_STORE_V4_SERVICE_INTENT.md section 11 on a
# GNSS-equipped RAK4631 (the collar) over USB. No power cut, no unplugging.
#
#   - after the upgrade the device is still AUTO (legacy GNSS rule);
#   - APP SERVICES T is saved and applied; ROLE BASE is refused;
#   - after a reboot the saved intent is used (ROLE? mode=SERVICES,
#     APP SERVICES? applied=EXPLICIT, tracking running);
#   - AUTO round trip, then T again.
#
# Usage, from the repository root, with the device's serial logger stopped:
#   sh firmware/tests/m7/services_remote_check.sh <DEVICE_ID_HEX>
#
# It checks out and flashes REF, so the working tree must be clean. It ends on
# branch "main" with the device running REF and services saved as T.
# Every device line it reads is kept in ~/services_check.log.
set -eu

# The checkout below replaces this file; keep running from a private copy.
if [ "${ORUN_SERVICES_CHECK_COPY:-}" != 1 ]; then
  copy=$(mktemp)
  cp "$0" "$copy"
  ORUN_SERVICES_CHECK_COPY=1 exec sh "$copy" "$@"
fi

ID=${1:?usage: sh $0 <DEVICE_ID_HEX>}
PORT="/dev/serial/by-id/usb-RAKwireless_WisCore_RAK4631_Board_${ID}-if00"
REF=${REF:-origin/feat/persistent-services}
LOG="$HOME/services_check.log"
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
  sleep 15  # boot, stores recovered, GNSS detected
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
  tail -n +"$((MARK + 1))" "$LOG" | tr -d '\r' |
    grep -E '^(APP SERVICES|APP SET|APP RESULT|ROLE)' | sed 's/^/     /'
}

flash "$REF"
ask "APP SERVICES?"
expect 'APP SERVICES mode=AUTO .*applied=LEGACY_AUTO .*legacy_mode=TRACKER' \
  "after the upgrade: AUTO, legacy rule, tracker"
ask "APP SERVICES T"
expect 'APP SET .*code=APPLIED services=T mode=EXPLICIT token=VALID' \
  "tracking saved"
ask "ROLE?"
expect 'ROLE TRACKER mode=SERVICES' "role follows the saved services"
ask "ROLE BASE"
expect 'ROLE command rejected: services are configured' "ROLE BASE refused"

flash "$REF"
ask "ROLE?"
expect 'ROLE TRACKER mode=SERVICES' "after reboot: role from saved services"
ask "APP SERVICES?"
expect 'mode=EXPLICIT requested=T applied=EXPLICIT tracking=(ON|DEGRADED) relay=OFF receive=OFF legacy_mode=TRACKER' \
  "after reboot: tracking runs from the saved intent"
ask "APP CONFIG?"
expect 'code=OK .*source=stored' "config still stored"

ask "APP SERVICES AUTO"
expect 'APP SET .*code=APPLIED services=AUTO mode=AUTO' "back to AUTO"
ask "ROLE?"
expect 'ROLE TRACKER mode=AUTO' "AUTO: GNSS present, legacy rule gives tracker"
ask "APP SERVICES T"
expect 'APP SET .*code=APPLIED services=T mode=EXPLICIT' "tracking saved again"

stop_listen
git checkout --quiet main
echo
if [ "$FAILURES" -eq 0 ]; then
  echo "RESULT: ALL PASS (device left on $REF, services T)"
else
  echo "RESULT: $FAILURES FAIL (device left on $REF; log: $LOG)"
fi
