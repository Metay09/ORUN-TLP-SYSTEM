#!/usr/bin/env bash
# Time-bounded, read-only BLE smoke test of the locally built BlueZ candidate.
# This BRIEFLY replaces ONLY host Bluetooth; host ORUN USB Edge and backend stay up.
# A systemd timer independently restores stock BlueZ if the terminal is lost.
set -euo pipefail
umask 077

REPO="$HOME/ORUN-PC-EDGE-VERIFY"
WORK="$HOME/ORUN-BLUEZ-566-HOTFIX"
CAND="$WORK/build-candidate.Hvqlif/src/bluetoothd"
EXPECTED="14412d0a913463f516d310c52f06a86ce0f5c6cc2748796733d596ac9bea4e05"
CAND_UNIT="orun-bluez-candidate.service"
TIMER_UNIT="orun-bluez-restore-$(date +%s)-$$"
BLE_ADDR="C5:DD:01:85:C9:4B"
SWITCHED=0

restore() {
  local result=$?
  trap - EXIT INT TERM HUP
  if [[ "$SWITCHED" == 1 ]]; then
    echo "=== RESTORE STOCK BLUETOOTH ==="
    sudo systemctl stop "$CAND_UNIT" || true
    sudo systemctl unmask --runtime bluetooth.service || true
    sudo systemctl start bluetooth.service || true
    sudo systemctl show bluetooth.service -p ActiveState -p MainPID --no-pager || true
    echo "Independent watchdog timer $TIMER_UNIT.timer also remains scheduled."
  fi
  echo "=== ORUN SERVIS CHECK ==="
  systemctl --user is-active orun-pc-edge.service || true
  systemctl --user is-active orun-backend.service || true
  exit "$result"
}
trap restore EXIT
trap 'exit 130' INT
trap 'exit 143' TERM HUP

echo "=== FAIL-CLOSED PREFLIGHT ==="
test -x "$CAND"
test -d "$REPO/tools"
test "$(sha256sum "$CAND" | awk '{print $1}')" = "$EXPECTED" || {
  echo "FAIL: candidate binary SHA differs from physically compiled file" >&2; exit 1;
}
test "$(systemctl is-active bluetooth.service)" = active || {
  echo "FAIL: stock Bluetooth daemon is not currently active" >&2; exit 1;
}
test "$(systemctl --user is-active orun-pc-edge.service)" = active
test "$(systemctl --user is-active orun-backend.service)" = active
systemctl --user show orun-pc-edge.service -p ExecStart | grep -q -- '--mode usb' || {
  echo "FAIL: Edge is not in USB mode, abort" >&2; exit 1;
}
systemctl is-active --quiet "$CAND_UNIT" && {
  echo "FAIL: a candidate already runs; abort" >&2; exit 1;
}
command -v systemd-run
command -v timeout
python3 - <<'PY'
import sys
sys.path.insert(0, "tools")
from bleak import BleakClient, BleakScanner
from orun_pc_edge import discover_ble_gateway
from orun_edge_bench import EdgeReassembler, EDGE_TO_EDGE_UUID
print("Python BLE probe dependencies: PASS")
PY
sudo -v

echo "=== REGISTER INDEPENDENT 100-SECOND ROLLBACK WATCHDOG ==="
# The root-owned transient timer will restore the installed system daemon
# even if this shell/SSH session is killed. It never starts/stops ORUN services.
sudo systemd-run --unit="$TIMER_UNIT" \
  --on-active=100s --timer-property=AccuracySec=1s \
  /usr/bin/bash -c \
  'systemctl stop orun-bluez-candidate.service >/dev/null 2>&1 || :; systemctl unmask --runtime bluetooth.service >/dev/null 2>&1 || :; systemctl start bluetooth.service'
sudo systemctl is-active --quiet "$TIMER_UNIT.timer" || {
  echo "FAIL: rollback watchdog timer not active; abort" >&2; exit 1;
}
echo "ROLLBACK WATCHDOG: ARMED ($TIMER_UNIT.timer)"

SWITCHED=1
echo "=== TEMPORARILY STOP ONLY STOCK BLUETOOTH ==="
sudo systemctl mask --runtime --now bluetooth.service
sudo systemctl is-enabled bluetooth.service | grep -qx masked

echo "=== RUN PATCHED bluetoothd (MAXIMUM 70 SEC) ==="
sudo systemd-run --unit=orun-bluez-candidate \
  --property=Type=simple --property=Restart=no \
  --property=RuntimeMaxSec=70s \
  "$CAND" -n
sleep 3
if ! sudo systemctl is-active --quiet "$CAND_UNIT"; then
  echo "FAIL: candidate daemon did not remain active"
  sudo journalctl -u "$CAND_UNIT" --no-pager -n 45
  exit 1
fi
echo "CANDIDATE: ACTIVE"
echo "=== READ-ONLY GATEWAY GATT PROBE ==="

# No write_gatt_char, no EDGE ACCEPT, no gateway firmware changes.
timeout 45s python3 - <<'PY'
import asyncio
import sys
sys.path.insert(0, "tools")
from bleak import BleakClient, BleakScanner
from orun_pc_edge import discover_ble_gateway
from orun_edge_bench import EDGE_TO_EDGE_UUID, EdgeReassembler

async def run():
    rx = EdgeReassembler()
    done = asyncio.Event()
    got = {"frames": 0, "hello": False, "dump": False}
    def receive(_, data):
        got["frames"] += 1
        packet = rx.push(bytes(data))
        if not packet:
            return
        if packet[0] == 1 and len(packet) == 13:
            gw = packet[1:9].hex().upper()
            got["hello"] = gw == "09A462BD4B275BA5"
            print("HELLO", gw, "valid", got["hello"], flush=True)
        elif packet[0] == 3 and len(packet) == 3:
            got["dump"] = True
            print("DUMP END", int.from_bytes(packet[1:3], "big"), flush=True)
        if got["hello"] and got["dump"]:
            done.set()
    try:
        dev = await discover_ble_gateway(BleakScanner, "C5:DD:01:85:C9:4B")
        print("DISCOVERED", bool(dev), flush=True)
        if dev:
            async with BleakClient(dev, timeout=15) as client:
                print("CONNECTED", client.is_connected, flush=True)
                await client.start_notify(EDGE_TO_EDGE_UUID, receive)
                try:
                    await asyncio.wait_for(done.wait(), timeout=15)
                except asyncio.TimeoutError:
                    print("GATT HELLO/DUMP timeout", flush=True)
    except Exception as exc:
        print("BLE ERROR", type(exc).__name__, str(exc), flush=True)
    print("FRAMES", got["frames"], flush=True)
    print("RESULT", "PASS" if done.is_set() else "FAIL", flush=True)

asyncio.run(run())
PY

echo "=== PATCHED BLUEZ LOGS ==="
sudo journalctl -u "$CAND_UNIT" --no-pager -n 45 || true
echo "=== END OF PROBE: RESTORE HANDLER WILL RESTART STOCK BLUETOOTH ==="
