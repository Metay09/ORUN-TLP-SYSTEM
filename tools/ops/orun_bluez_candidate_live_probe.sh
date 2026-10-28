#!/usr/bin/env bash
# A 45-second read-only BLE test of the patched BlueZ daemon.
# Only host Bluetooth is switched temporarily. The original is restored on exit.
# An independent systemd timer restores it even if this shell is killed.
set -euo pipefail
umask 077

REPO="$HOME/ORUN-PC-EDGE-VERIFY"
CAND="$HOME/ORUN-BLUEZ-566-HOTFIX/build-candidate.Hvqlif/src/bluetoothd"
HASH="14412d0a913463f516d310c52f06a86ce0f5c6cc2748796733d596ac9bea4e05"
UNIT="orun-bluez-candidate.service"
WATCH="orun-bluez-restore-$(date +%s)-$$"
CHANGED=0

restore() {
  local rc=$?
  trap - EXIT INT TERM HUP
  if [[ "$CHANGED" == 1 ]]; then
    echo "=== RESTORE ORIGINAL BLUETOOTH ==="
    sudo systemctl stop "$UNIT" || true
    sudo systemctl unmask --runtime bluetooth.service || true
    sudo systemctl start bluetooth.service || true
    if sudo systemctl is-active --quiet bluetooth.service; then
      # Once stock is confirmed up, our failover timer is no longer needed.
      sudo systemctl stop "$WATCH.timer" || true
    fi
  fi
  echo "=== FINAL SERVICE STATES ==="
  systemctl is-active bluetooth.service || true
  systemctl --user is-active orun-pc-edge.service || true
  systemctl --user is-active orun-backend.service || true
  echo "=== INDEPENDENT ROLLBACK TIMER: $WATCH.timer ==="
  exit "$rc"
}
trap restore EXIT
trap 'exit 130' INT
trap 'exit 143' TERM HUP

echo "=== PRECHECK ==="
test -x "$CAND"
test -f "$HOME/ORUN-BLUEZ-566-HOTFIX/orun_bluez_candidate_ble_probe.py"
test -f "$HOME/ORUN-BLUEZ-566-HOTFIX/orun_pc_edge.py"
test "$(sha256sum "$CAND" | awk '{print $1}')" = "$HASH"
test "$(systemctl is-active bluetooth.service)" = active
test "$(systemctl --user is-active orun-pc-edge.service)" = active
test "$(systemctl --user is-active orun-backend.service)" = active
systemctl --user show orun-pc-edge.service -p ExecStart |
  grep -q -- '--mode usb'
if systemctl is-active --quiet "$UNIT"; then
  echo "ERROR: a candidate daemon is already running" >&2
  exit 1
fi
python3 -m py_compile "$HOME/ORUN-BLUEZ-566-HOTFIX/orun_bluez_candidate_ble_probe.py"
python3 -m py_compile "$HOME/ORUN-BLUEZ-566-HOTFIX/orun_pc_edge.py"
sudo -v

echo "=== RETIRE OLD COMPLETED-TRIAL ROLLBACK TIMERS ==="
# Only after proving stock Bluetooth active and no candidate daemon running.
# Otherwise an old 100-second timer could interrupt this new candidate trial.
while IFS= read -r old_unit; do
  case "$old_unit" in
    orun-bluez-restore-*.timer)
      echo "RETIRED OLD TIMER: $old_unit"
      sudo systemctl stop "$old_unit"
      ;;
  esac
done < <(systemctl list-units --type=timer --state=active --plain --no-legend   'orun-bluez-restore-*.timer' | awk '{print $1}')

echo "=== ARM INDEPENDENT 100-SECOND ROLLBACK TIMER ==="
sudo systemd-run --unit="$WATCH" --on-active=100s \
  --timer-property=AccuracySec=1s \
  /usr/bin/bash -c \
  'systemctl stop orun-bluez-candidate.service >/dev/null 2>&1 || :; systemctl unmask --runtime bluetooth.service >/dev/null 2>&1 || :; systemctl start bluetooth.service'
sudo systemctl is-active --quiet "$WATCH.timer"
echo "ROLLBACK: ARMED"

CHANGED=1
echo "=== SWITCH ONLY BLUETOOTH ==="
sudo systemctl mask --runtime --now bluetooth.service

echo "=== START PATCHED DAEMON FOR AT MOST 70 SECONDS ==="
sudo systemd-run --unit=orun-bluez-candidate \
  --property=Type=simple --property=Restart=no \
  --property=RuntimeMaxSec=70s \
  "$CAND" -n
sleep 3
if ! sudo systemctl is-active --quiet "$UNIT"; then
  echo "ERROR: candidate daemon exited early"
  sudo journalctl -u "$UNIT" --no-pager -n 35
  exit 1
fi

echo "=== READ-ONLY GATT PROBE ==="
RC=0
timeout 45s env ORUN_PC_EDGE_REPO="$REPO" python3 "$HOME/ORUN-BLUEZ-566-HOTFIX/orun_bluez_candidate_ble_probe.py" || RC=$?
echo "BLE PROBE EXIT CODE: $RC"

echo "=== PATCHED BLUEZ DIAGNOSTICS ==="
sudo journalctl -u "$UNIT" --no-pager -n 35 || true
exit "$RC"
