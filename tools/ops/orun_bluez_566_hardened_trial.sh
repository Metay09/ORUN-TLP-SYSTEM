#!/usr/bin/env bash
# BlueZ 5.66 hotfix: stage outside /home, then bounded hardened-service BLE trial.
# Trial restores stock bluetoothd on EXIT and via independent root timer.
# Never writes to dpkg-owned daemon, Gateway custody, PC Edge or Backend.
set -euo pipefail
umask 077

MODE="$*"
WORK="$HOME/ORUN-BLUEZ-566-HOTFIX"
CAND="$WORK/build-candidate.Hvqlif/src/bluetoothd"
HASH="14412d0a913463f516d310c52f06a86ce0f5c6cc2748796733d596ac9bea4e05"
STAGE="/usr/local/libexec/orun-bluez/bluetoothd-5.66-gattfix"
VENDOR="/usr/libexec/bluetooth/bluetoothd"
DROP="/run/systemd/system/bluetooth.service.d/80-orun-gattfix-trial.conf"
TIMER="orun-bluez-hardened-restore-$(date +%s)-$$"
CHANGED=0

host_check() {
  test -x "$CAND" && test -x "$VENDOR"
  test "$(sha256sum "$CAND" | cut -d' ' -f1)" = "$HASH"
  test "$("$CAND" --version)" = 5.66
  test "$(dpkg-query -W bluez | awk '{print $2}')" = '5.66-1+deb12u2'
  test "$(systemctl is-active bluetooth.service)" = active
  test "$(systemctl --user is-active orun-pc-edge.service)" = active
  test "$(systemctl --user is-active orun-backend.service)" = active
  test "$(systemctl show bluetooth.service -p Type --value)" = dbus
  test "$(systemctl show bluetooth.service -p ProtectHome --value)" = yes
  test -z "$(systemctl show bluetooth.service -p DropInPaths --value)"
  systemctl show bluetooth.service -p ExecStart | grep -Fq "$VENDOR"
  test ! -e "$DROP"
  test -f "$WORK/orun_bluez_candidate_ble_probe.py"
  test -f "$WORK/orun_pc_edge.py"
  python3 -m py_compile "$WORK/orun_bluez_candidate_ble_probe.py" "$WORK/orun_pc_edge.py"
  python3 -c 'import bleak'
}

restore() {
  rc=$?
  trap - EXIT HUP INT TERM
  if [ "$CHANGED" = 1 ]; then
    echo '=== RESTORE ORIGINAL BLUETOOTH ==='
    sudo rm -f -- "$DROP" || true
    sudo systemctl daemon-reload || true
    sudo systemctl restart bluetooth.service || true
    if sudo systemctl is-active --quiet bluetooth.service &&
       systemctl show bluetooth.service -p ExecStart | grep -Fq "$VENDOR"; then
      sudo systemctl stop "$TIMER.timer" || true
      echo 'RESTORE: PASS'
    else
      echo 'RESTORE NEEDS ATTENTION: watchdog still armed' >&2
      rc=1
    fi
  fi
  echo '=== SERVICE STATES ==='
  systemctl is-active bluetooth.service || true
  systemctl --user is-active orun-pc-edge.service || true
  systemctl --user is-active orun-backend.service || true
  exit "$rc"
}

case "$MODE" in
  stage)
    host_check
    sudo -v
    echo '=== STAGE ONLY; BLUETOOTH UNCHANGED ==='
    sudo install -d -o root -g root -m 0755 /usr/local/libexec/orun-bluez
    if sudo test -e "$STAGE"; then
      test "$(sudo sha256sum "$STAGE" | cut -d' ' -f1)" = "$HASH"
    else
      sudo install -o root -g root -m 0755 "$CAND" "$STAGE"
    fi
    test "$(sudo sha256sum "$STAGE" | cut -d' ' -f1)" = "$HASH"
    echo 'STAGE: PASS'
    echo "VENDOR SHA256: $(sha256sum "$VENDOR" | cut -d' ' -f1)"
    echo 'Bluetooth service was NOT restarted.'
    ;;
  trial)
    host_check
    sudo -v
    test "$(sudo sha256sum "$STAGE" | cut -d' ' -f1)" = "$HASH"
    if systemctl list-units --type=timer --state=active --plain --no-legend \
      'orun-bluez-hardened-restore-*.timer' | grep -Fq 'orun-bluez-hardened-restore-'; then
      echo 'Previous rollback timer still active. Abort.' >&2
      exit 1
    fi
    trap restore EXIT
    trap 'exit 130' INT
    trap 'exit 143' HUP TERM
    echo '=== ARM INDEPENDENT 120-SECOND RESTORE ==='
    sudo systemd-run --unit="$TIMER" --on-active=120s \
      --timer-property=AccuracySec=1s /usr/bin/bash -c \
      'rm -f /run/systemd/system/bluetooth.service.d/80-orun-gattfix-trial.conf; systemctl daemon-reload; systemctl restart bluetooth.service'
    CHANGED=1
    sudo systemctl is-active --quiet "$TIMER.timer"
    echo '=== TEMPORARY /run SERVICE DROP-IN ==='
    sudo install -d -o root -g root -m 0755 /run/systemd/system/bluetooth.service.d
    printf '%s\n' '[Service]' 'ExecStart=' "ExecStart=$STAGE" | sudo tee "$DROP" >/dev/null
    sudo chmod 0644 "$DROP"
    sudo systemctl daemon-reload
    systemctl show bluetooth.service -p ExecStart | grep -Fq "$STAGE"
    test "$(systemctl show bluetooth.service -p Type --value)" = dbus
    test "$(systemctl show bluetooth.service -p ProtectHome --value)" = yes
    echo '=== ACTIVATE CANDIDATE IN EXISTING HARDENED UNIT ==='
    sudo systemctl restart bluetooth.service
    sudo systemctl is-active --quiet bluetooth.service
    systemctl show bluetooth.service -p ExecStart -p MainPID --no-pager
    systemctl --user is-active --quiet orun-pc-edge.service
    systemctl --user is-active --quiet orun-backend.service
    echo '=== READ-ONLY BLE HELLO/DUMP ==='
    timeout 50s env ORUN_PC_EDGE_REPO="$HOME/ORUN-PC-EDGE-VERIFY" \
      python3 "$WORK/orun_bluez_candidate_ble_probe.py"
    echo 'HARDENED BLUEZ BLE: PASS'
    echo '=== VENDOR RESTORE FOLLOWS ON EXIT ==='
    ;;
  *)
    echo "Usage: bash $0 stage|trial" >&2
    exit 2 ;;
esac
