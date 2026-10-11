#!/bin/sh
# A/B check on the collar: GNSS backup sleep (meant for hot starts) against
# cutting the 3V3_S rail, in the same place and the same hour.
#
# The serial logger keeps running. This script only writes the RAM-only
# "GNSS SLEEP <min>" setting to the port, alternating every 6 minutes
# (0 = cut the rail, 240 = the default backup sleep), then reads the logger's
# file. Each TTFF is attributed to the sleep line printed before its ACQUIRE:
#   "GNSS backup sleep ..."          -> BACKUP
#   "GNSS low power (3V3_S release)" -> RAILCUT
# so the exact moment of switching does not matter. The default (240) is
# restored at the end, also on Ctrl+C.
#
# Usage, from the repository root:
#   sh firmware/tests/m3/gnss_sleep_ab.sh <DEVICE_ID_HEX> [minutes] [logfile]
# Defaults: 60 minutes, ~/tasma.log. Works with the 3-minute interval; with a
# long interval there are too few attempts to compare.
set -eu

ID=${1:?usage: sh $0 <DEVICE_ID_HEX> [minutes] [logfile]}
MINUTES=${2:-60}
LOG=${3:-$HOME/tasma.log}
PORT="/dev/serial/by-id/usb-RAKwireless_WisCore_RAK4631_Board_${ID}-if00"

[ -e "$PORT" ] || { echo "No device at $PORT"; exit 1; }
[ -f "$LOG" ] || { echo "No log file $LOG (is the logger running?)"; exit 1; }

send() { printf '%s\n' "$1" > "$PORT"; }
restore() { send "GNSS SLEEP 240" 2>/dev/null || true; }
trap 'restore; exit 130' INT TERM

start=$(($(wc -l < "$LOG") + 1))
end=$(($(date +%s) + MINUTES * 60))
echo "Running $MINUTES min; switching every 6 min. Ctrl+C stops early."
cut=1
while [ "$(date +%s)" -lt "$end" ]; do
  if [ "$cut" -eq 1 ]; then send "GNSS SLEEP 0"; cut=0; else send "GNSS SLEEP 240"; cut=1; fi
  sleep 360
done
restore
trap - INT TERM

tail -n +"$start" "$LOG" | tr -d '\r' | awk '
  /GNSS: detected/ { mode = "" }  # a reboot: the next fix is a boot start
  /GNSS backup sleep/ { mode = "BACKUP" }
  /GNSS low power \(3V3_S release\)/ { mode = "RAILCUT" }
  /GNSS ACQUIRE start/ { current = mode }
  /GNSS FIX ttff=/ && current != "" {
    match($0, /ttff=[0-9]+/)
    print current, substr($0, RSTART + 5, RLENGTH - 5)
    current = ""
  }
  /GNSS ACQUIRE timeout/ && current != "" { print current, "TIMEOUT"; current = "" }
' > "$LOG.ab"

echo
for mode in RAILCUT BACKUP; do
  times=$(awk -v m="$mode" '$1 == m && $2 != "TIMEOUT" { print $2 }' "$LOG.ab" | sort -n)
  fixes=$(printf '%s\n' "$times" | grep -c '[0-9]' || true)
  timeouts=$(awk -v m="$mode" '$1 == m && $2 == "TIMEOUT"' "$LOG.ab" | wc -l)
  median=$(printf '%s\n' "$times" | grep '[0-9]' | awk '
    { v[NR] = $1 }
    END {
      if (NR == 0) { print "-"; exit }
      if (NR % 2) print int(v[(NR + 1) / 2] / 1000)
      else print int((v[NR / 2] + v[NR / 2 + 1]) / 2000)
    }')
  all=$(printf '%s\n' "$times" | grep '[0-9]' | awk '{ printf "%s%d", sep, $1 / 1000; sep = " " }')
  echo "$mode: fixes=$fixes timeouts=$timeouts median_ttff_s=$median all_s=[$all]"
done
echo "(raw: $LOG.ab; GNSS SLEEP restored to 240)"
