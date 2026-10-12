#!/bin/sh
# SF5F step 1 on the collar: the production image stores each finished report
# period durably (ProductRecordStore on the product region) and keeps the
# records and their numbering across a reboot. Over USB, no unplugging.
#
#   1. flash the current checkout; PRODUCT STORE? must show the region
#      mounted (a development device whose region only holds leftovers is
#      prepared first with PRODUCT STORE PREPARE CONFIRM, which then reboots);
#   2. remember the tracking interval, set the shortest one (60 s);
#   3. three periods must each end with "PRODUCT STORE append ... result=OK"
#      and consecutive sequence numbers;
#   4. reboot (flash again): the store mounts with those records and the next
#      period continues the numbering;
#   5. the original tracking interval is restored.
#
# Usage, from the repository root, with the device's logger stopped:
#   sh firmware/tests/sf5/sf5f_records_remote_check.sh <DEVICE_ID_HEX>
# Device lines go to ~/sf5f_records.log. Part of firmware/tests/device_check.sh.
set -eu

ID=${1:?usage: sh $0 <DEVICE_ID_HEX>}
PORT="/dev/serial/by-id/usb-RAKwireless_WisCore_RAK4631_Board_${ID}-if00"
LOG="$HOME/sf5f_records.log"
READER=""
MARK=0
INTERVAL0=""

[ -e "$PORT" ] || { echo "Cihaz yok: $PORT"; exit 1; }
if pgrep -f "orunlog.sh $ID" > /dev/null 2>&1; then
  echo "Bu cihazin logger'i calisiyor. Once durdur:  pkill -f \"orunlog.sh $ID\""
  exit 1
fi
: > "$LOG"

listen() {
  for _ in $(seq 1 60); do
    [ -e "$PORT" ] && break
    sleep 0.5
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

since_mark() { tail -n +"$((MARK + 1))" "$LOG" | tr -d '\r'; }

wait_seen() {  # regex, seconds
  for _ in $(seq 1 "$2"); do
    since_mark | grep -q -E "$1" && return 0
    sleep 1
  done
  return 1
}

field() {  # line-prefix regex, key
  since_mark | grep -E "^$1" | tail -n 1 | tr ' ' '\n' |
    sed -n "s/^$2=//p" | head -n 1
}

send() {
  MARK=$(wc -l < "$LOG")
  printf '%s\n' "$1" > "$PORT"
}

restore_interval() {
  if [ -n "$INTERVAL0" ]; then
    send "APP INTERVAL $INTERVAL0"
    wait_seen '^APP SET .*tracking_interval_seconds=' 10 || true
  fi
}

fail() {
  echo
  echo "RESULT: FAIL ($1)"
  since_mark | grep -E '^PRODUCT ' | tail -n 6
  restore_interval
  echo "Log: $LOG"
  exit 1
}

# Production prints before USB is open, so ask instead of reading boot lines.
store_status() {
  sleep 8
  for _ in 1 2 3; do
    send "PRODUCT STORE?"
    wait_seen '^PRODUCT STORE region=' 5 && return 0
  done
  fail "no answer to PRODUCT STORE?"
}

boot() {  # flash the current checkout, then read the mount state
  echo "== yukleniyor: production"
  stop_listen
  pio run -d firmware -e rak4630 -t upload --upload-port "$PORT" \
    > "$LOG.build" 2>&1 || { echo "FAIL build/upload (see $LOG.build)"; exit 1; }
  sleep 3
  listen
  store_status
}

boot
if [ "$(field 'PRODUCT STORE region' decision)" = NEEDS_PREPARATION ]; then
  echo "     bolgede eski veri var; hazirlaniyor (PREPARE)"
  send "PRODUCT STORE PREPARE CONFIRM"
  wait_seen '^PRODUCT STORE PREPARE result=PASS' 60 || fail "prepare"
  stop_listen
  sleep 4
  listen
  store_status
fi
[ "$(field 'PRODUCT STORE region' mounted)" = yes ] || fail "region not mounted"
echo "     once: $(since_mark | grep '^PRODUCT STORE ready=' | tail -n 1)"
sleep 10  # GNSS detected, tracking scheduled

send "APP CONFIG?"
wait_seen 'tracking_interval_seconds=' 10 || fail "APP CONFIG?"
INTERVAL0=$(field 'APP RESULT' tracking_interval_seconds)
[ -n "$INTERVAL0" ] || fail "APP CONFIG? interval missing"
echo "     once: $(since_mark | grep '^PRODUCT STORE ready=' | tail -n 1)"
send "APP INTERVAL 60"
wait_seen '^APP SET .*code=(APPLIED|UNCHANGED) tracking_interval_seconds=60' 10 ||
  fail "APP INTERVAL 60"

# 3. Three stored periods with consecutive numbers.
MARK=$(wc -l < "$LOG")
for _ in $(seq 1 1200); do
  [ "$(since_mark | grep -c '^PRODUCT STORE append .*result=OK')" -ge 3 ] && break
  since_mark | grep -q '^PRODUCT STORE append .*result=FAIL' && fail "append FAIL"
  sleep 1
done
appends=$(since_mark | grep '^PRODUCT STORE append .*result=OK' | head -n 3)
[ "$(printf '%s\n' "$appends" | grep -c .)" -eq 3 ] || fail "3 records not stored in 20 min"
printf '%s\n' "$appends" | sed 's/^/     /'
seqs=$(printf '%s\n' "$appends" | sed -n 's/.* seq=\([0-9]*\) .*/\1/p')
first=$(printf '%s\n' "$seqs" | head -n 1)
last=$(printf '%s\n' "$seqs" | tail -n 1)
[ "$last" -eq $((first + 2)) ] || fail "sequence numbers not consecutive: $seqs"
since_mark | grep -q '^PRODUCT PERIODIC .*store=ACCEPTED' || fail "no accepted period line"

# 4. Reboot: records kept, numbering continues. A period may still be stored
# while the upload starts, so take the newest stored number from the log, and
# allow one more that committed just before the reset.
STEP3=$MARK
boot
newest=$(sed -n "$((STEP3 + 1)),$((MARK))p" "$LOG" | tr -d '\r' |
  sed -n 's/^PRODUCT STORE append seq=\([0-9]*\) .*result=OK.*/\1/p' | tail -n 1)
[ "$(field 'PRODUCT STORE region' mounted)" = yes ] || fail "not mounted after reboot"
wait_seen '^PRODUCT STORE ready=' 5 || fail "PRODUCT STORE? after reboot"
echo "     yeniden baslatma sonrasi: $(since_mark | grep '^PRODUCT STORE ready=' | tail -n 1)"
[ "$(field 'PRODUCT STORE ready' faulted)" = no ] || fail "store faulted after reboot"
next=$(field 'PRODUCT STORE ready' next_seq)
[ "$next" -ge $((newest + 1)) ] && [ "$next" -le $((newest + 2)) ] ||
  fail "next_seq $next after reboot, newest stored before it $newest"
MARK=$(wc -l < "$LOG")
wait_seen '^PRODUCT STORE append .*result=(OK|FAIL)' 300 || fail "no record after reboot"
since_mark | grep -q "^PRODUCT STORE append seq=$next .*result=OK" ||
  fail "record after reboot: $(since_mark | grep '^PRODUCT STORE append' | tail -n 1)"

# 5. Original interval back.
restore_interval
echo "     takip araligi geri alindi: ${INTERVAL0}s"
stop_listen
echo
echo "RESULT: PASS (kayitlar $first..$next kalici; cihaz production'da)"
echo "Logger'i tekrar baslat:  sh ~/orunlog.sh $ID >> ~/tasma.log &"
