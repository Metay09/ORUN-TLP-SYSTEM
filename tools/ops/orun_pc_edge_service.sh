#!/usr/bin/env bash
# Opt-in Debian 12 user service for the *PC Edge* collector/uploader.
# Never changes firmware, starts/stops the backend, or erases Gateway records.
set -euo pipefail
umask 077
SELF="$(readlink -f "${BASH_SOURCE[0]}")"
ROOT="$(cd "$(dirname "$SELF")/../.." && pwd)"
TOKEN_FILE="$HOME/.config/orun/pc-edge.token"
UNIT_DIR="$HOME/.config/systemd/user"
UNIT_FILE="$UNIT_DIR/orun-pc-edge.service"

usage() {
  echo "Kullanim:" >&2
  echo "  bash tools/ops/orun_pc_edge_service.sh install usb http://100.x.x.x:8090 pc-adi /dev/serial/by-id/GATEWAY" >&2
  echo "  bash tools/ops/orun_pc_edge_service.sh install ble http://100.x.x.x:8090 pc-adi" >&2
  echo "  bash tools/ops/orun_pc_edge_service.sh validate usb URL EDGE-ID /dev/serial/by-id/GATEWAY" >&2
  echo "  bash tools/ops/orun_pc_edge_service.sh status|logs" >&2
  exit 2
}

# Parse arguments without relying on Bash/locale-dependent regex matching.
# Also provide a read-only validation entrypoint for host regression tests.
validate_install_args() {
  if (( $# < 3 || $# > 4 )); then
    echo "HATA: install icin mod, URL, Edge ID ve USB icin port gerekir." >&2
    return 2
  fi
  python3 - "$1" "$2" "$3" "${4:-}" <<'PY'
import re
import sys

mode, url, edge_id, port = sys.argv[1:]
if mode not in ("usb", "ble"):
    sys.exit("HATA: Baglanti modu usb veya ble olmali.")
if not re.fullmatch(r"[A-Za-z0-9._-]{1,64}", edge_id, flags=re.ASCII):
    sys.exit("HATA: Edge ID 1-64 ASCII harf/rakam/._- olabilir.")
if mode == "usb":
    if not re.fullmatch(r"/dev/(?:ttyACM[0-9]+|serial/by-id/[A-Za-z0-9_.:-]+)", port,
                        flags=re.ASCII):
        sys.exit("HATA: USB portu /dev/serial/by-id/ veya /dev/ttyACM[0-9] olmali.")
elif port:
    sys.exit("HATA: BLE modunda USB portu verilmez.")
PY
}

install_service() {
  local mode="${1:-}" url="${2:-}" edge_id="${3:-}" port="${4:-}"
  validate_install_args "$@" || return 2
  if [[ "$ROOT" == *" "* || "$ROOT" == *"%"* ]]; then
    echo "HATA: Proje yolunda bosluk veya % karakteri var." >&2
    exit 1
  fi
  command -v systemctl >/dev/null || { echo "systemd gerekli" >&2; exit 1; }
  python3 - "$url" "$ROOT" <<'PY'
import sys
sys.path.insert(0, sys.argv[2] + "/tools")
from orun_pc_edge import checked_backend
assert checked_backend(sys.argv[1]) == sys.argv[1].rstrip("/")
PY
  if [[ "$mode" == usb ]]; then
    python3 -c 'import serial' || {
      echo "HATA: USB icin python3-serial/pyserial kurulmali." >&2
      exit 1
    }
  else
    python3 -c 'import bleak' || {
      echo "HATA: BLE icin bleak/BlueZ kurulmali." >&2
      exit 1
    }
  fi
  if [[ -e "$UNIT_FILE" ]]; then
    echo "HATA: PC Edge service zaten var, ustune yazilmadi: $UNIT_FILE" >&2
    exit 1
  fi
  if [[ ! -s "$TOKEN_FILE" ]]; then
    if [[ ! -t 0 ]]; then
      echo "HATA: Mevcut backend Edge tokenini girilecek terminal gerekli." >&2
      exit 1
    fi
    mkdir -p "$(dirname "$TOKEN_FILE")"
    chmod 700 "$(dirname "$TOKEN_FILE")"
    local token
    IFS= read -r -s -p "Backend ile ayni ORUN Edge bearer token: " token
    printf '\n'
    if (( ${#token} < 8 )) || [[ "$token" == *$'\n'* || "$token" == *$'\r'* ]]; then
      echo "HATA: en az 8 karakter tek satir token olmali." >&2
      exit 1
    fi
    printf '%s\n' "$token" > "$TOKEN_FILE"
    unset token
  fi
  chmod 600 "$TOKEN_FILE"
  mkdir -p "$UNIT_DIR"
  local args="--mode $mode --backend-url $url --edge-id $edge_id --token-file $TOKEN_FILE"
  if [[ "$mode" == usb ]]; then
    args="$args --port $port"
  fi
  cat > "$UNIT_FILE" <<EOF
[Unit]
Description=ORUN PC Edge (sealed USB/BLE spool to tailnet backend)
Documentation=https://github.com/Metay09/ORUN-TLP-SYSTEM

[Service]
Type=simple
WorkingDirectory=$ROOT
ExecStart=/usr/bin/python3 -u $ROOT/tools/orun_pc_edge.py $args
Restart=always
RestartSec=10
UMask=0077
NoNewPrivileges=true

[Install]
WantedBy=default.target
EOF
  systemctl --user daemon-reload
  systemctl --user enable --now orun-pc-edge.service
  echo "PC Edge acildi, log:"
  systemctl --user status orun-pc-edge.service --no-pager || true
  echo "Backend URL: $url (ayri bilgisayarda olabilir)"
  echo "Gateway kayitlarini silen veya kabul/onay veren komut yoktur."
  echo "Oturum acilmadan otomatik baslamasi icin bir kez:"
  echo "sudo loginctl enable-linger $(id -un)"
}

case "${1:-}" in
  install) shift; install_service "$@" ;;
  validate) shift; validate_install_args "$@" ;;
  status) systemctl --user status orun-pc-edge.service --no-pager || true ;;
  logs) journalctl --user -u orun-pc-edge.service -n 80 --no-pager ;;
  *) usage ;;
esac
