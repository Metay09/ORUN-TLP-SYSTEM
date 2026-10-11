#!/bin/sh
# Finds which accelerometer axis runs along the neck toward the head on the
# assembled collar (activity_classifier.h, NeckAxis). Two still poses held by
# hand: the box as it sits on the neck with the head level, then the same box
# with its head-side edge tilted down about 45 degrees. The axis whose reading
# changes most is the neck axis; it points toward the head when its reading
# drops as the head side goes down.
#
# The serial logger keeps running; this script writes commands to the port and
# reads the logger's file, like gnss_sleep_ab.sh. At the end it sends
# "ACTIVITY AXIS <axis>" (RAM only, lost at reboot) and prints the result.
#
# Usage, from the repository root:
#   sh firmware/tests/m6/activity_axis_bench.sh <DEVICE_ID_HEX> [logfile]
set -eu

ID=${1:?device id}
LOG=${2:-$HOME/tasma.log}
PORT="/dev/serial/by-id/usb-RAKwireless_WisCore_RAK4631_Board_${ID}-if00"

[ -e "$PORT" ] || { echo "Cihaz yok: $PORT"; exit 1; }
[ -f "$LOG" ] || { echo "Log dosyasi yok: $LOG (logger calisiyor mu?)"; exit 1; }

send() { printf '%s\n' "$1" > "$PORT"; }

# Last "ACTIVITY POSE ... g_mg=x,y,z" written after line $1 of the log.
read_pose() {
  tail -n +"$1" "$LOG" | tr -d '\r' | awk '
    /ACTIVITY POSE/ { if (match($0, /g_mg=-?[0-9]+,-?[0-9]+,-?[0-9]+/))
                        last = substr($0, RSTART + 5, RLENGTH - 5) }
    END { print last }'
}

capture() {
  printf '%s\n' "$1"
  printf 'Hazir olunca Enter: '
  read -r _
  echo "15 saniye kipirdatmadan tut..."
  sleep 15
  start=$(($(wc -l < "$LOG") + 1))
  send "ACTIVITY AXIS?"
  sleep 2
  pose=$(read_pose "$start")
  [ -n "$pose" ] || { echo "Cevap gelmedi. Logger ve tasma calisiyor mu?"; exit 1; }
  echo "  okunan: $pose"
}

send "ACTIVITY AUTO ON"
sleep 3

capture "1) Tasmayi boynunda duracagi gibi tut: gunes paneli yukari, kutu duz (bas dik duruyormus gibi)."
LEVEL=$pose
capture "2) Simdi kutunun HAYVANIN BASINA bakacak kenarini asagi eg, yaklasik 45 derece."
TILT=$pose

AXIS=$(printf '%s %s\n' "$LEVEL" "$TILT" | awk '{
  split($1, a, ","); split($2, b, ",")
  name[1] = "X"; name[2] = "Y"; name[3] = "Z"
  best = 0; bestd = 0
  for (i = 1; i <= 3; i++) {
    d = b[i] - a[i]; ad = d < 0 ? -d : d
    if (ad > bestd) { bestd = ad; best = i; sign = d < 0 ? "+" : "-" }
  }
  if (bestd < 300) { print "NONE"; exit }
  print sign name[best]
}')

if [ "$AXIS" = "NONE" ]; then
  echo "Egim yeterince gorulmedi (300 mg alti). Daha fazla egip tekrar dene."
  exit 1
fi

send "ACTIVITY AXIS $AXIS"
sleep 2
echo
echo "SONUC: boyun ekseni $AXIS"
echo "Bunu bana yaz. Simdilik ayar tasmada yeniden baslatana kadar gecerli."
