#!/bin/sh
# SF5E: physical ObservationStore qualification on the product region
# [0x0C5000, 0x0E5000) of a DEVELOPMENT RAK4631, over USB, no unplugging.
#
#  1. flash rak4630_sf5e_qual (test-only); boot is read-only;
#  2. PREPARE (erase only the product region), append, verify, reboot;
#  3. software cuts around single store writes/erases (BEFORE, TORN = half a
#     write, AFTER = written but unseen), each followed by a reboot that must
#     mount healthy, keep every committed record intact and contiguous,
#     finish maintenance and append again: appends, release markers, page
#     maintenance before the store wraps and page reclaim after it wraps;
#  4. check that Geofence/Security/Config/BLE bond/History and the bootloader
#     code are byte-identical to the start (CRC32);
#  5. PREPARE again so no test records stay behind, then flash production.
# A real power cut needs the operator at the device and is not part of this
# script. Device lines go to ~/sf5e.log.
#
# Usage, from the repository root on main, with that device's logger stopped:
#   sh firmware/tests/sf5/sf5e_qual_remote.sh <DEVICE_ID_HEX>
set -eu

if [ "${ORUN_SF5E_COPY:-}" != 1 ]; then
  copy=$(mktemp)
  cp "$0" "$copy"
  ORUN_SF5E_COPY=1 exec sh "$copy" "$@"
fi

ID=${1:?usage: sh $0 <DEVICE_ID_HEX>}
PORT="/dev/serial/by-id/usb-RAKwireless_WisCore_RAK4631_Board_${ID}-if00"
LOG="$HOME/sf5e.log"
READER=""
MARK=0
CUTS=0

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

seen() { since_mark | grep -q -E "$1"; }

wait_seen() {  # regex, seconds
  for _ in $(seq 1 "$(($2 * 10))"); do
    seen "$1" && return 0
    sleep 0.1
  done
  return 1
}

# Value of key=... on the newest line starting with the prefix since MARK.
field() {
  since_mark | grep -E "^$1" | tail -n 1 | tr ' ' '\n' |
    sed -n "s/^$2=//p" | head -n 1
}

flash() {
  echo "== yukleniyor: $2"
  stop_listen
  pio run -d firmware -e "$1" -t upload --upload-port "$PORT" \
    > "$LOG.build" 2>&1 || { echo "FAIL build/upload of $2 (see $LOG.build)"; exit 1; }
  sleep 3
  MARK=$(wc -l < "$LOG")
  listen
}

restore() {
  flash rak4630 "production"
  sleep 10
  stop_listen
}

fail() {
  echo
  echo "RESULT: FAIL ($1)"
  echo "Son satirlar:"
  since_mark | tail -n 15
  restore
  echo "Log: $LOG   Logger'i tekrar baslat:  sh ~/orunlog.sh $ID >> ~/tasma.log &"
  exit 1
}

# After a reset (cut or REBOOT): wait for USB to come back and the probe to
# finish its read-only boot.
reconnect() {
  stop_listen
  sleep 2
  for _ in 1 2; do
    MARK=$(wc -l < "$LOG")
    listen
    if wait_seen '^SF5E READY' 25; then
      [ "$(field 'SF5E STORE' mounted)" = yes ] || fail "mount after reset"
      [ "$(field 'SF5E STORE' faulted)" = no ] || fail "store faulted after reset"
      return 0
    fi
    stop_listen
    sleep 2
  done
  fail "device did not come back after reset"
}

# Send one command and wait for its terminal line. Returns 0 when the line
# arrived, 2 when the device reset instead (a cut fired, or REBOOT).
run() {
  MARK=$(wc -l < "$LOG")
  printf '%s\n' "$1" > "$PORT"
  for _ in $(seq 1 1200); do
    if seen '^SF5E CUT fired|^SF5E REBOOT'; then
      reconnect
      return 2
    fi
    seen "$2" && return 0
    sleep 0.1
  done
  fail "timeout: $1"
}

pass() {  # command, terminal regex, result-line prefix
  run "$1" "$2" || fail "unexpected reset: $1"
  [ "$(field "$3" result)" = PASS ] || fail "$1"
}

verify() { pass "SF5E VERIFY" '^SF5E VERIFY ' 'SF5E VERIFY'; }

# One cut around one command, then the post-reset checks. Sets DELTA to the
# number of records the interrupted command committed. Returns 1 when the
# command finished without reaching the armed call.
cut_case() {  # setup-command-or-empty, command, call, mode
  verify
  if [ -n "$1" ]; then
    pass "$1" '^SF5E (FILLPAGE|APPEND|MAINT|RELEASE) ' 'SF5E (FILLPAGE|APPEND|MAINT|RELEASE)'
  fi
  verify
  last1=$(field 'SF5E VERIFY' last)
  run "SF5E CUT $3 $4" '^SF5E CUT armed' || fail "arm"
  if run "$2" '^SF5E CUT not_reached'; then
    return 1
  fi
  CUTS=$((CUTS + 1))
  verify
  recovered=$(field 'SF5E VERIFY' last)
  pass "SF5E MAINT" '^SF5E MAINT ' 'SF5E MAINT'
  pass "SF5E APPEND 1" '^SF5E APPEND ' 'SF5E APPEND'
  verify
  last2=$(field 'SF5E VERIFY' last)
  DELTA=$((recovered - last1))
  [ "$DELTA" -ge 0 ] && [ "$DELTA" -le 1 ] || fail "$2 cut $3 $4: last $last1 -> $recovered"
  [ "$last2" -eq $((recovered + 1)) ] || fail "$2 cut $3 $4: append after recovery"
  return 0
}

maintenance_matrix() {
  for mode in BEFORE TORN AFTER; do
    k=1
    while [ "$k" -le 24 ]; do
      cut_case "SF5E FILLPAGE" "SF5E MAINT" "$k" "$mode" || break
      k=$((k + 1))
    done
    [ "$k" -gt 1 ] || fail "maintenance $mode: no cut point reached"
    echo "     $1: bakim $mode, $((k - 1)) kesme noktasi PASS"
  done
}

# 1. Test image; read-only boot.
flash rak4630_sf5e_qual "SF5E qualification (TEST-ONLY)"
wait_seen '^SF5E READY' 30 || fail "qualification image did not boot"
echo "     bolge: $(since_mark | grep '^SF5E REGION' | tail -n 1)"
PROTECT0=$(since_mark | grep '^SF5E PROTECT' | tail -n 1)
[ -n "$PROTECT0" ] || fail "no PROTECT line"

# 2. Prepare, append, verify, reboot.
pass "SF5E PREPARE CONFIRM" '^SF5E PREPARE result=' 'SF5E PREPARE result'
pass "SF5E APPEND 10" '^SF5E APPEND ' 'SF5E APPEND'
verify
[ "$(field 'SF5E VERIFY' n)" -eq 10 ] || fail "10 records expected"
run "SF5E REBOOT" '^SF5E READY' && fail "REBOOT did not reset"
verify
[ "$(field 'SF5E VERIFY' n)" -eq 10 ] || fail "records lost across reboot"
echo "     yaz / oku / yeniden baslat: PASS"

# 3a. Append: body write = call 1, commit word = call 2.
for c in "1 BEFORE 0" "1 TORN 0" "1 AFTER 0" "2 BEFORE 0" "2 AFTER 1"; do
  # shellcheck disable=SC2086 # split "call mode delta" on purpose
  set -- $c
  cut_case "" "SF5E APPEND 1" "$1" "$2" || fail "append cut $1 $2 not reached"
  [ "$DELTA" -eq "$3" ] || fail "append cut $1 $2: committed $DELTA, expected $3"
done
echo "     kayit yazma: 5 kesme noktasi PASS"

# 3b. Release marker.
for mode in BEFORE AFTER; do
  verify
  rel0=$(field 'SF5E VERIFY' rel)
  run "SF5E CUT 1 $mode" '^SF5E CUT armed' || fail "arm"
  run "SF5E RELEASE 1" '^SF5E CUT not_reached' && fail "release cut not reached"
  CUTS=$((CUTS + 1))
  verify
  expected=$rel0
  [ "$mode" = AFTER ] && expected=$((rel0 + 1))
  [ "$(field 'SF5E VERIFY' rel)" -eq "$expected" ] || fail "release cut $mode"
  pass "SF5E RELEASE 1" '^SF5E RELEASE ' 'SF5E RELEASE'
done
echo "     teslim isareti: 2 kesme noktasi PASS"

# 3c. Page maintenance before the store wraps.
maintenance_matrix "sarmadan once"

# 3d. Fill until the oldest page is reclaimed, then cut reclaim.
reclaimed=0
for _ in $(seq 1 40); do
  pass "SF5E FILLPAGE" '^SF5E FILLPAGE ' 'SF5E FILLPAGE'
  pass "SF5E MAINT" '^SF5E MAINT ' 'SF5E MAINT'
  reclaimed=$(field 'SF5E DIAG' reclaimed)
  [ "$reclaimed" -gt 0 ] && break
done
[ "$reclaimed" -gt 0 ] || fail "store never wrapped"
echo "     doldurma: en eski sayfa geri alindi (reclaimed=$reclaimed)"
maintenance_matrix "sardiktan sonra"

# 4. Nothing outside the product region changed.
verify
run "SF5E STATUS" '^SF5E PROTECT' || fail "status reset"
PROTECT1=$(since_mark | grep '^SF5E PROTECT' | tail -n 1)
[ "$PROTECT1" = "$PROTECT0" ] || fail "another region changed: $PROTECT0 / $PROTECT1"
echo "     diger bolgeler ve acilis yazilimi: degismedi PASS"

# 5. Leave no test records behind; production back.
pass "SF5E PREPARE CONFIRM" '^SF5E PREPARE result=' 'SF5E PREPARE result'
verify
[ "$(field 'SF5E VERIFY' n)" -eq 0 ] || fail "region not empty after cleanup"
restore

echo
echo "RESULT: PASS ($CUTS kesme + yeniden baslatma; cihaz production'da)"
echo "Logger'i tekrar baslat:  sh ~/orunlog.sh $ID >> ~/tasma.log &"
