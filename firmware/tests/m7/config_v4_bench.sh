#!/bin/sh
# Guided bench run of the remaining physical gates of
# ORUN_CONFIG_STORE_V4_SERVICE_INTENT.md section 9 on a USB-powered RAK4631
# without a battery (unplugging USB is a real power cut):
#
#   item 1: blank ConfigStore, fresh v4 baseline, cold boot, cold boot again;
#   item 3: normal save cut after erase, after body+CRC, after commit
#           (operator unplugs at each "CUT POWER NOW");
#   item 4: a save with a phone connected over BLE (SoftDevice async path),
#           recovered after a power cut; interval restored to 180 s.
#
# Uses the TEST-ONLY probes rak4630_config_v2_qual_probe and
# rak4630_config_v2_normal_save_powercut_probe (v4-aware despite the names),
# then the production image. ERASES the device's ConfigStore (interval and
# services return to defaults); History, geofence, security and bonds stay.
#
# Usage, from the repository root, with that device's serial logger stopped:
#   sh firmware/tests/m7/config_v4_bench.sh <DEVICE_ID_HEX>
# Stops at the first failed check. Device lines: ~/config_bench.log.
set -eu

if [ "${ORUN_CONFIG_BENCH_COPY:-}" != 1 ]; then
  copy=$(mktemp)
  cp "$0" "$copy"
  ORUN_CONFIG_BENCH_COPY=1 exec sh "$copy" "$@"
fi

ID=${1:?usage: sh $0 <DEVICE_ID_HEX>}
PORT="/dev/serial/by-id/usb-RAKwireless_WisCore_RAK4631_Board_${ID}-if00"
REF=${REF:-origin/feat/power-services-status}
LOG="$HOME/config_bench.log"
BLE_NAME="ORUN-$(printf '%s' "$ID" | tail -c 8)"
READER=""
MARK=0

[ -e "$PORT" ] || { echo "No device at $PORT"; exit 1; }
if pgrep -f "orunlog.sh $ID" > /dev/null 2>&1; then
  echo "Bu cihazin logger'i calisiyor. Once durdur:  pkill -f \"orunlog.sh $ID\""
  exit 1
fi
if ! git diff --quiet || ! git diff --cached --quiet; then
  echo "Working tree has local changes; commit or stash first."
  exit 1
fi
git fetch --quiet origin
git checkout --quiet --detach "$REF"
: > "$LOG"

listen() {
  for _ in $(seq 1 30); do
    [ -e "$PORT" ] && break
    sleep 1
  done
  # Right after a replug udev may still be setting up the node.
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

finish() {
  stop_listen
  echo
  echo "$1"
  echo "Log: $LOG"
  exit "$2"
}

trap 'stop_listen' EXIT

since_mark() {
  tail -n +"$((MARK + 1))" "$LOG" | tr -d '\r' | grep -E "$1" || true
}

# wait_for <regex> <seconds>: true once a line since the mark matches.
wait_for() {
  i=0
  while [ "$i" -lt "$2" ]; do
    [ -n "$(since_mark "$1" | head -n 1)" ] && return 0
    sleep 1
    i=$((i + 1))
  done
  return 1
}

ask() {
  MARK=$(wc -l < "$LOG")
  printf '%s\n' "$1" > "$PORT"
}

# check <regex> <description>
check() {
  if [ -n "$(since_mark "$1" | head -n 1)" ]; then
    echo "PASS $2"
  else
    echo "FAIL $2"
    since_mark '^(CONFIG V2|APP |BLE )' | sed 's/^/     /'
    finish "RESULT: FAIL at: $2" 1
  fi
}

show() { since_mark "$1" | sed 's/^/     /'; }

# ready <regex|PRODUCTION>: the probes wait for the USB host and print a
# READY line; the production image does not wait, so it is polled with BLE?.
# A probe that booted before the port was opened has printed into the void;
# STATUS makes it report again (the checks only need STORE/TOKEN/PAGE).
ready() {
  if [ "$1" != PRODUCTION ]; then
    wait_for "$1" 20 && return 0
    ask "STATUS"
    wait_for '^CONFIG V2 (NORMAL )?PAGE B' 10
    return
  fi
  sleep 5
  for _ in $(seq 1 20); do
    ask "BLE?"
    wait_for '^BLE ready=' 2 && return 0
  done
  return 1
}

# flash <env> <description> <ready regex|PRODUCTION>
flash() {
  echo "== yukleniyor: $2"
  stop_listen
  pio run -d firmware -e "$1" -t upload --upload-port "$PORT" \
    > "$LOG.build" 2>&1 || finish "FAIL build/upload of $2 (see $LOG.build)" 1
  sleep 3
  MARK=$(wc -l < "$LOG")
  listen
  ready "$3" || finish "FAIL $2 did not report after upload" 1
}

# power_cycle <ready regex|PRODUCTION>
power_cycle() {
  stop_listen
  echo
  echo ">>> Alicinin USB kablosunu CIKAR, sonra Enter'a bas."
  read -r _ < /dev/tty
  while [ -e "$PORT" ]; do
    echo "    Kablo hala takili gorunuyor. Cikar, sonra Enter'a bas."
    read -r _ < /dev/tty
  done
  echo ">>> 5 saniye bekle ve kabloyu geri TAK (Enter gerekmez)."
  MARK=$(wc -l < "$LOG")
  for _ in $(seq 1 300); do
    [ -e "$PORT" ] && break
    sleep 0.5
  done
  [ -e "$PORT" ] || finish "FAIL device did not come back on $PORT" 1
  listen
  ready "$1" || finish "FAIL no boot report after the power cut" 1
}

token_of() {  # incarnation hex from the last TOKEN line since the mark
  since_mark 'TOKEN incarnation=0x' | tail -n 1 |
    sed -n 's/.*incarnation=0x\([0-9A-F]*\) .*/\1/p'
}

QUAL_READY='^CONFIG V2 QUAL READY'
NORMAL_READY='^CONFIG V2 NORMAL READY'

echo "== 1/3: bos depo, ilk kayit, soguk acilis"
flash rak4630_config_v2_qual_probe "ConfigStore helper (TEST-ONLY)" "$QUAL_READY"
show '^CONFIG V2 (STORE|TOKEN|PAGE)'
ask "CLEAN"
wait_for '^CONFIG V2 CLEAN (PASS|FAIL)' 20 || true
check '^CONFIG V2 CLEAN PASS pages=2 all_ff=yes' "ConfigStore erased and verified"
power_cycle "$QUAL_READY"
show '^CONFIG V2 (STORE|TOKEN|PAGE)'
check 'STORE ready=yes maintenance=no token_state=VALID .*baseline_commits=1 ' \
  "item 1: fresh baseline written once"
check '^CONFIG V2 TOKEN incarnation=0x[0-9A-F]{16} revision=1$' "baseline token revision 1"
check '^CONFIG V2 PAGE A evidence=V4_COMMITTED .*revision=1 ' "page A: committed v4 record"
check '^CONFIG V2 PAGE B evidence=ERASED' "page B: erased"
INCARNATION=$(token_of)
[ "$INCARNATION" != "0000000000000000" ] || finish "RESULT: FAIL zero incarnation" 1
power_cycle "$QUAL_READY"
check 'STORE ready=yes maintenance=no token_state=VALID .*baseline_commits=0 ' \
  "item 1: second cold boot recovers, no new baseline"
check "^CONFIG V2 TOKEN incarnation=0x$INCARNATION revision=1\$" "same incarnation after cold boot"

echo
echo "== 2/3: kayit sirasinda elektrik kesintisi (3 nokta)"
flash rak4630_config_v2_normal_save_powercut_probe "normal-save cut probe (TEST-ONLY)" "$NORMAL_READY"
check 'NORMAL STORE ready=yes maintenance=no token_state=VALID' "probe sees the v4 baseline"

ask "CUT_ERASE"
wait_for 'CUT READY stage=after-erase-readback' 20 || true
check 'CUT READY stage=after-erase-readback' "cut point after erase reached"
power_cycle "$NORMAL_READY"
show '^CONFIG V2 NORMAL (STORE|TOKEN|PAGE)'
check 'NORMAL STORE ready=yes maintenance=no token_state=VALID .*battery_capacity_mah=0 ' \
  "item 3a: cut after erase keeps the old config"
check "NORMAL TOKEN incarnation=0x$INCARNATION revision=1\$" "item 3a: token stays revision 1"
check 'NORMAL PAGE A evidence=V4_COMMITTED' "item 3a: page A committed"
check 'NORMAL PAGE B evidence=ERASED' "item 3a: page B erased"

ask "CUT_BODY"
wait_for 'CUT READY stage=after-body-readback' 20 || true
check 'CUT READY stage=after-body-readback' "cut point after body reached"
power_cycle "$NORMAL_READY"
show '^CONFIG V2 NORMAL (STORE|TOKEN|PAGE)'
check 'NORMAL STORE ready=yes maintenance=no token_state=VALID .*battery_capacity_mah=0 ' \
  "item 3b: cut after body keeps the old config"
check "NORMAL TOKEN incarnation=0x$INCARNATION revision=1\$" "item 3b: token stays revision 1"
check 'NORMAL PAGE B evidence=V4_STAGED .*revision=2 ' "item 3b: page B staged successor, not promoted"

ask "CUT_COMMIT"
wait_for 'CUT READY stage=after-commit-readback' 20 || true
check 'CUT READY stage=after-commit-readback' "cut point after commit reached"
power_cycle "$NORMAL_READY"
show '^CONFIG V2 NORMAL (STORE|TOKEN|PAGE)'
check 'NORMAL STORE ready=yes maintenance=no token_state=VALID committed_override=yes .*battery_capacity_mah=1 ' \
  "item 3c: cut after commit recovers the new config"
check "NORMAL TOKEN incarnation=0x$INCARNATION revision=2\$" "item 3c: token revision 2"
check 'NORMAL PAGE B evidence=V4_COMMITTED .*revision=2 ' "item 3c: page B committed"

echo
echo "== 3/3: telefon BLE ile bagliyken kayit"
flash rak4630 "$REF (production)" PRODUCTION
echo
echo ">>> Telefonda nRF Connect uygulamasini ac, $BLE_NAME cihazina CONNECT de."
echo "    Baglaninca bekle, betik kendisi gorecek (en fazla 5 dakika)."
connected=no
for _ in $(seq 1 60); do
  ask "BLE?"
  if wait_for '^BLE ready=yes .*connected=1' 5; then connected=yes; break; fi
done
[ "$connected" = yes ] || finish "RESULT: FAIL no BLE connection seen" 1
echo "PASS phone connected over BLE"
ask "APP INTERVAL 300"
wait_for '^APP SET .*code=' 20 || true
check '^APP SET .*code=APPLIED tracking_interval_seconds=300 token=VALID' \
  "item 4: save applied with BLE connected"
ask "BLE?"
wait_for '^BLE ' 5 || true
check '^BLE ready=yes .*connected=1' "item 4: phone still connected after the save"
echo "    (Telefondaki baglantiyi simdi kesebilirsin.)"
power_cycle PRODUCTION
ask "APP CONFIG?"
wait_for '^APP RESULT .*source=' 10 || true
check '^APP RESULT .*code=OK .*source=stored tracking_interval_seconds=300 ' \
  "item 4: saved value recovered after a power cut"
ask "APP INTERVAL 180"
wait_for '^APP SET .*code=' 20 || true
check '^APP SET .*code=APPLIED tracking_interval_seconds=180 ' "interval restored to 180 s"

finish "RESULT: ALL PASS (items 1, 3, 4; device left on $REF, interval 180 s).
Logger'i tekrar baslat:  nohup sh ~/orunlog.sh $ID >> ~/alici.log 2>&1 &" 0
