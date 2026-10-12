#!/bin/sh
# Every automatic device check, in one command, on one development RAK4631
# with GNSS (the collar), over USB, no unplugging. Each check flashes what it
# needs and ends on the production image of the current checkout.
#
#   1. CC310 crypto KAT                 sf5/sf5b_crypto_kat_remote.sh
#   2. saved services (T / AUTO / reboot) m7/services_remote_check.sh
#   3. USB geofence writer              m6/geofence_usb_remote_check.sh
#   4. firmware update keeps the product region   sf5/sf5d_full_retention_remote.sh
#   5. ObservationStore on real flash with cut points   sf5/sf5e_qual_remote.sh
#   6. production stores each period, keeps it across a reboot
#                                       sf5/sf5f_records_remote_check.sh
#
# The device ends on production with services T, NO geofence, its tracking
# interval unchanged and a product region holding only the few records of
# check 6 (checks 4 and 5 write test data there and erase the region,
# including any earlier records).
# Not included, because they need hands or hours: activity_axis_bench.sh
# (tilt the box), config_v4_bench.sh (unplug), gnss_sleep_ab.sh (hours).
#
# Usage, from the repository root on a clean, up-to-date main, with the
# device's logger stopped:
#   sh firmware/tests/device_check.sh <DEVICE_ID_HEX>
# Output of each check: ~/device_check_<n>.out; summary: ~/device_check.txt.
set -u

ID=${1:?usage: sh $0 <DEVICE_ID_HEX>}
PORT="/dev/serial/by-id/usb-RAKwireless_WisCore_RAK4631_Board_${ID}-if00"
SUMMARY="$HOME/device_check.txt"

[ -e "$PORT" ] || { echo "Cihaz yok: $PORT"; exit 1; }
if pgrep -f "orunlog.sh $ID" > /dev/null 2>&1; then
  echo "Bu cihazin logger'i calisiyor. Once durdur:  pkill -f \"orunlog.sh $ID\""
  exit 1
fi
if ! git diff --quiet || ! git diff --cached --quiet; then
  echo "Yerel degisiklik var; once commit veya stash yap."
  exit 1
fi
HEAD=$(git rev-parse --short HEAD)
BRANCH=$(git rev-parse --abbrev-ref HEAD)

CHECKS="sf5/sf5b_crypto_kat_remote.sh|Sifreleme (CC310)
m7/services_remote_check.sh|Servis ayari
m6/geofence_usb_remote_check.sh|Otlak siniri yazma
sf5/sf5d_full_retention_remote.sh|Guncellemede kayit alani korunur
sf5/sf5e_qual_remote.sh|Kayit yazma ve kesmeler
sf5/sf5f_records_remote_check.sh|Normal yazilim kaydi saklar"

started=$(date +%s)
{
  echo "ORUN cihaz testleri  $(date '+%Y-%m-%d %H:%M')  cihaz=$ID  kod=$BRANCH@$HEAD"
  echo
} > "$SUMMARY"
n=0
echo "$CHECKS" | while IFS='|' read -r script title; do
  n=$((n + 1))
  out="$HOME/device_check_$n.out"
  echo "== $n/6 $title"
  t0=$(date +%s)
  # stdin closed: nothing a check runs may eat the list of checks.
  sh "firmware/tests/$script" "$ID" > "$out" 2>&1 < /dev/null
  code=$?
  minutes=$(( ($(date +%s) - t0 + 30) / 60 ))
  result=$(grep '^RESULT:' "$out" | tail -n 1)
  verdict=FAIL
  case "$result" in
    *FAIL*) ;;
    *PASS*) [ "$code" -eq 0 ] && verdict=PASS ;;
  esac
  [ -n "$result" ] || result="RESULT yok (cikis kodu $code), son satir: $(tail -n 1 "$out")"
  printf '%-2s %-34s %-4s %3s dk  %s\n' "$n" "$title" "$verdict" "$minutes" "$result" |
    tee -a "$SUMMARY"
  # If a check could not even build/upload, later checks would only repeat
  # the same failure: stop here.
  if grep -q '^FAIL build/upload' "$out"; then
    echo "   derleme/yukleme hatasi; ayrinti: $out" | tee -a "$SUMMARY"
    break
  fi
done

total=$(grep -c -E '^[0-9] ' "$SUMMARY")
passed=$(grep -c -E '^[0-9] .* PASS ' "$SUMMARY")
elapsed=$(( ($(date +%s) - started + 30) / 60 ))
{
  echo
  if [ "$total" -eq 6 ] && [ "$passed" -eq 6 ]; then
    echo "SONUC: 6/6 PASS ($elapsed dk)"
  else
    echo "SONUC: $passed/6 PASS, $((total - passed)) FAIL, $((6 - total)) calismadi ($elapsed dk)"
  fi
} | tee -a "$SUMMARY"
echo "Ozet: $SUMMARY   Logger'i tekrar baslat:  sh ~/orunlog.sh $ID >> ~/tasma.log &"
[ "$total" -eq 6 ] && [ "$passed" -eq 6 ]
