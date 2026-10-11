#!/bin/sh
# RAK4631 KAT of the SF5B ProductSecureCrypto seam over USB, no unplugging
# (ORUN_TLP_V2_PRODUCT_SECURE_WIRE.md section 21, gate 4):
#
#   1. flash the TEST-ONLY rak4630_sf5b_product_crypto_probe image: at boot,
#      after Bluefruit.begin(), it runs the M7P6I History KAT (control) and the
#      SF5B PRODUCT_SECURE KAT on CC310 with RAM-backed SecurityStores and the
#      public test credential, and repeats both result lines every 3 s;
#   2. capture both lines;
#   3. flash the production image of REF back.
#
# The physical SecurityStore, config, geofence and History are not touched.
#
# Usage, from the repository root, with that device's serial logger stopped:
#   sh firmware/tests/sf5/sf5b_crypto_kat_remote.sh <DEVICE_ID_HEX>
# It checks out REF (detached) and leaves it checked out. Device lines go to
# ~/sf5b_kat.log.
set -eu

if [ "${ORUN_SF5B_KAT_COPY:-}" != 1 ]; then
  copy=$(mktemp)
  cp "$0" "$copy"
  ORUN_SF5B_KAT_COPY=1 exec sh "$copy" "$@"
fi

ID=${1:?usage: sh $0 <DEVICE_ID_HEX>}
PORT="/dev/serial/by-id/usb-RAKwireless_WisCore_RAK4631_Board_${ID}-if00"
REF=${REF:-origin/feat/sf5b-product-codec}
LOG="$HOME/sf5b_kat.log"
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

flash() {
  echo "== yukleniyor: $2"
  stop_listen
  pio run -d firmware -e "$1" -t upload --upload-port "$PORT" \
    > "$LOG.build" 2>&1 || { echo "FAIL build/upload of $2 (see $LOG.build)"; exit 1; }
  sleep 3
  MARK=$(wc -l < "$LOG")
  listen
}

last_line() {
  tail -n +"$((MARK + 1))" "$LOG" | tr -d '\r' | grep -E "$1" | tail -n 1 || true
}

flash rak4630_sf5b_product_crypto_probe "SF5B crypto KAT probe (TEST-ONLY)"
sf5b=""
for _ in $(seq 1 40); do
  sf5b=$(last_line '^SF5B PRODUCT CRYPTO KAT ')
  [ -n "$sf5b" ] && break
  sleep 1
done
history=$(last_line '^M7P6I HISTORY CRYPTO KAT ')
echo "     ${history:-no M7P6I line}"
echo "     ${sf5b:-no SF5B line}"

flash rak4630 "$REF (production)"
sleep 10
stop_listen

case "$sf5b" in
  "SF5B PRODUCT CRYPTO KAT PASS "*)
    echo; echo "RESULT: PASS (device back on $REF production)" ;;
  *)
    echo; echo "RESULT: FAIL (device back on $REF production; log: $LOG)"; exit 1 ;;
esac
echo "Logger'i tekrar baslat:  nohup sh ~/orunlog.sh $ID >> ~/alici.log 2>&1 &"
