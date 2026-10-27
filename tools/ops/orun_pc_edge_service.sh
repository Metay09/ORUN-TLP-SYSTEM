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
  echo "  bash tools/ops/orun_pc_edge_service.sh status|logs" >&2
  exit 2
}

install_service() {
  local mode="${1:-}" url="${2:-}" edge_id="${3:-}" port="${4:-}"
  [[ "$mode" == usb || "$mode" == ble ]] || usage
  [[ "$edge_id" =~ ^[A-Za-z0-9._-]{1,64}$ ]] || usage
  if [[ "$mode" == usb ]]; then
    if [[ ! "$port" =~ ^/dev/(ttyACM[0-9]+|serial/by-id/[A-Za-z0-9_.:-]+)$ ]]; then
      echo "HATA: Sabit Gateway seri port yolunu verin (/dev/serial/by-id onerilir)." >&2
      exit 1
    fi
  elif [[ -n "$port" ]]; then
    usage
  fi
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
  status) systemctl --user status orun-pc-edge.service --no-pager || true ;;
  logs) journalctl --user -u orun-pc-edge.service -n 80 --no-pager ;;
  *) usage ;;
esac
