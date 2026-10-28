#!/usr/bin/env bash
# ORUN BlueZ 5.66 reversible hardened systemd integration.
# install: preflight, root-owned recovery, 120s watchdog, real BLE read-only qualification.
# rollback: restore Debian binary and remove our own drop-in.
# Neither command stops PC Edge USB or Backend, replaces vendor binary, or touches firmware.
set -euo pipefail
umask 077

MODE="$1"
WORK="$HOME/ORUN-BLUEZ-566-HOTFIX"
CAND="/usr/local/libexec/orun-bluez/bluetoothd-5.66-gattfix"
HASH="14412d0a913463f516d310c52f06a86ce0f5c6cc2748796733d596ac9bea4e05"
VENDOR="/usr/libexec/bluetooth/bluetoothd"
VHASH="ea290bd384a9930cd4e091c411cab00d56de8cb78ff0a62f45cc170232ab8be8"
DROP="/etc/systemd/system/bluetooth.service.d/80-orun-gattfix.conf"
RECOVER="/usr/local/libexec/orun-bluez/restore-vendor.sh"
RECOVERY_UNIT="/etc/systemd/system/orun-bluez-automatic-recovery.service"
TIMER="orun-bluez-install-restore-$(date +%s)-$$"
SWITCHED=0
TIMER_ARMED=0

service_states() {
  echo "=== SERVICE STATE ==="
  systemctl show bluetooth.service -p ActiveState -p ExecStart -p DropInPaths --no-pager
  systemctl --user is-active orun-pc-edge.service || true
  systemctl --user is-active orun-backend.service || true
}

restore_vendor() {
  sudo rm -f -- "$DROP"
  sudo systemctl daemon-reload
  sudo systemctl reset-failed bluetooth.service || true
  sudo systemctl restart bluetooth.service
  sudo systemctl is-active --quiet bluetooth.service
  systemctl show bluetooth.service -p ExecStart | grep -Fq "$VENDOR"
}

cleanup_on_exit() {
  rc=$?
  trap - EXIT HUP INT TERM
  if [ "$SWITCHED" = 1 ]; then
    echo "=== FAILSAFE: RESTORE DEBIAN BLUETOOTH ==="
    restore_vendor || rc=1
  fi
  if [ "$TIMER_ARMED" = 1 ] &&
     sudo systemctl is-active --quiet bluetooth.service &&
     systemctl show bluetooth.service -p ExecStart | grep -Fq "$VENDOR"; then
    sudo systemctl stop "$TIMER.timer" || true
  fi
  service_states
  exit "$rc"
}

preflight() {
  test -x "$CAND" && test -x "$VENDOR"
  test "$(sudo sha256sum "$CAND" | awk '{print $1}')" = "$HASH"
  test "$(sha256sum "$VENDOR" | awk '{print $1}')" = "$VHASH"
  test "$(dpkg-query -W bluez | awk '{print $2}')" = '5.66-1+deb12u2'
  test "$(systemctl is-active bluetooth.service)" = active
  test "$(systemctl --user is-active orun-pc-edge.service)" = active
  test "$(systemctl --user is-active orun-backend.service)" = active
  test "$(systemctl show bluetooth.service -p Type --value)" = dbus
  test "$(systemctl show bluetooth.service -p ProtectHome --value)" = yes
  test -z "$(systemctl show bluetooth.service -p DropInPaths --value)"
  systemctl show bluetooth.service -p ExecStart | grep -Fq "$VENDOR"
  sudo test ! -e "$DROP"
  sudo test ! -e "$RECOVER"
  sudo test ! -e "$RECOVERY_UNIT"
  test -f "$WORK/orun_bluez_candidate_ble_probe.py"
  test -f "$WORK/orun_pc_edge.py"
  python3 -m py_compile "$WORK/orun_bluez_candidate_ble_probe.py" "$WORK/orun_pc_edge.py"
  python3 -c 'import bleak'
  if systemctl list-units --type=timer --state=active --plain --no-legend \
    'orun-bluez-install-restore-*.timer' | grep -Fq 'orun-bluez-install-restore-'; then
    echo 'Old install rollback timer is still armed: abort' >&2
    exit 1
  fi
}

prepare_recovery() {
  local temp
  temp="$(mktemp "$WORK/bluetooth-restore.XXXXXXXX")"
  cat > "$temp" <<'RESTORE'
#!/usr/bin/env bash
set -euo pipefail
drop="/etc/systemd/system/bluetooth.service.d/80-orun-gattfix.conf"
vendor="/usr/libexec/bluetooth/bluetoothd"
test -x "$vendor"
rm -f -- "$drop"
systemctl daemon-reload
systemctl reset-failed bluetooth.service || true
systemctl start bluetooth.service
systemctl is-active --quiet bluetooth.service
systemctl show bluetooth.service -p ExecStart | grep -Fq "$vendor"
RESTORE
  sudo install -o root -g root -m 0755 "$temp" "$RECOVER"
  rm -f "$temp"
  temp="$(mktemp "$WORK/bluetooth-recovery-unit.XXXXXXXX")"
  cat > "$temp" <<'UNIT'
[Unit]
Description=ORUN BlueZ failure - restore Debian bluetoothd
[Service]
Type=oneshot
ExecStart=/usr/local/libexec/orun-bluez/restore-vendor.sh
UNIT
  sudo install -o root -g root -m 0644 "$temp" "$RECOVERY_UNIT"
  rm -f "$temp"
}

do_install() {
  sudo -v
  preflight
  prepare_recovery
  trap cleanup_on_exit EXIT
  trap 'exit 130' INT
  trap 'exit 143' TERM HUP

  echo "=== ARM INDEPENDENT 120s ROLLBACK ==="
  sudo systemd-run --unit="$TIMER" --on-active=120s \
    --timer-property=AccuracySec=1s /usr/bin/bash -c \
    'rm -f /etc/systemd/system/bluetooth.service.d/80-orun-gattfix.conf; systemctl daemon-reload; systemctl reset-failed bluetooth.service; systemctl restart bluetooth.service'
  sudo systemctl is-active --quiet "$TIMER.timer"
  TIMER_ARMED=1

  echo "=== INSTALL PERSISTENT SYSTEMD OVERRIDE ==="
  sudo install -d -o root -g root -m 0755 /etc/systemd/system/bluetooth.service.d
  local temp
  temp="$(mktemp "$WORK/bluetooth-override.XXXXXXXX")"
  cat > "$temp" <<'CONF'
[Unit]
OnFailure=orun-bluez-automatic-recovery.service
[Service]
ExecStart=
ExecStart=/usr/local/libexec/orun-bluez/bluetoothd-5.66-gattfix
CONF
  sudo install -o root -g root -m 0644 "$temp" "$DROP"
  rm -f "$temp"
  SWITCHED=1
  sudo systemctl daemon-reload
  systemctl show bluetooth.service -p ExecStart | grep -Fq "$CAND"
  test "$(systemctl show bluetooth.service -p Type --value)" = dbus
  test "$(systemctl show bluetooth.service -p ProtectHome --value)" = yes

  echo "=== ACTIVATE CANDIDATE ==="
  sudo systemctl restart bluetooth.service
  sudo systemctl is-active --quiet bluetooth.service
  systemctl show bluetooth.service -p ExecStart | grep -Fq "$CAND"

  echo "=== REAL READ-ONLY GATT HELLO/DUMP ==="
  timeout 50s env ORUN_PC_EDGE_REPO="$HOME/ORUN-PC-EDGE-VERIFY" \
    python3 "$WORK/orun_bluez_candidate_ble_probe.py"
  echo 'PHYSICAL GATT: PASS'

  systemctl --user is-active --quiet orun-pc-edge.service
  systemctl --user is-active --quiet orun-backend.service
  sudo systemctl is-active --quiet bluetooth.service
  systemctl show bluetooth.service -p ExecStart | grep -Fq "$CAND"

  echo 'PERSISTENT INSTALL: PASS'
  SWITCHED=0
  sudo systemctl stop "$TIMER.timer"
  TIMER_ARMED=0
  trap - EXIT INT TERM HUP
  service_states
  echo 'OnFailure automatic restoration is armed; Debian binary unchanged.'
}

do_rollback() {
  sudo -v
  test -x "$VENDOR"
  sudo test -f "$DROP"
  sudo grep -Fxq "ExecStart=$CAND" "$DROP"
  echo '=== RESTORE VENDOR AND REMOVE CUSTOM CONFIG ==='
  restore_vendor
  sudo rm -f -- "$RECOVERY_UNIT" "$RECOVER"
  sudo systemctl daemon-reload
  service_states
  echo 'ROLLBACK: PASS'
}

case "$MODE" in
  install) do_install ;;
  rollback) do_rollback ;;
  status) service_states ;;
  *)
    echo "Usage: bash $0 install|rollback|status" >&2
    exit 2 ;;
esac
