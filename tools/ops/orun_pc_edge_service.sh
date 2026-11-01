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
  echo "  bash tools/ops/orun_pc_edge_service.sh bench-release validate 16HEX-GATEWAY-ID" >&2
  echo "  bash tools/ops/orun_pc_edge_service.sh bench-release enable 16HEX-GATEWAY-ID" >&2
  echo "  bash tools/ops/orun_pc_edge_service.sh bench-release disable" >&2
  echo "  bash tools/ops/orun_pc_edge_service.sh transport validate ble C5:DD:01:85:C9:4B" >&2
  echo "  bash tools/ops/orun_pc_edge_service.sh transport ble C5:DD:01:85:C9:4B" >&2
  echo "  bash tools/ops/orun_pc_edge_service.sh transport usb" >&2
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

# Keep the installed receive-only unit unchanged. An explicit, reversible
# systemd drop-in enables the SF5L0 PUBLIC-TEST-KEY handoff on USB only.
# The Gateway validates the actual HMAC; this is not a production receipt.
validate_bench_gateway() {
  if (( $# != 1 )); then
    echo "HATA: 16 hexadecimal gateway kimligi gerekli." >&2
    return 2
  fi
  python3 - "$1" <<'PY'
import re
import sys
if not re.fullmatch(r"[0-9A-F]{16}", sys.argv[1], flags=re.ASCII):
    sys.exit("HATA: Gateway kimligi tam 16 buyuk hexadecimal karakter olmali.")
PY
}

bench_release() {
  local action="${1:-}"
  if [[ "$action" == "validate" ]]; then
    validate_bench_gateway "${@:2}"
    return
  fi
  if [[ "$action" == "enable" ]]; then
    validate_bench_gateway "${@:2}"
  elif [[ "$action" == "disable" ]]; then
    if (( $# != 1 )); then usage; fi
  else
    usage
  fi

  if [[ ! -f "$UNIT_FILE" ]]; then
    echo "HATA: PC Edge servisi once kurulmus olmali." >&2
    return 1
  fi
  local exec_line
  exec_line="$(sed -n 's/^ExecStart=//p' "$UNIT_FILE")"
  if [[ "$exec_line" != "/usr/bin/python3 -u $ROOT/tools/orun_pc_edge.py "* ||
        "$exec_line" != *" --mode usb "* ||
        "$exec_line" != *" --token-file $TOKEN_FILE"* ||
        "$exec_line" != *" --port /dev/"* ||
        "$exec_line" == *"--bench-release-gateway"* ]]; then
    echo "HATA: Kurulu USB PC Edge servis komutu beklenen bicimde degil; degistirilmedi." >&2
    return 1
  fi

  local dropdir="$UNIT_DIR/orun-pc-edge.service.d"
  local dropin="$dropdir/10-bench-release.conf"
  if [[ "$action" == "enable" ]]; then
    if [[ -e "$dropin" ]]; then
      echo "HATA: Bench teslim modu zaten ayarlanmis. Yeniden yazilmadi." >&2
      return 1
    fi
    mkdir -p "$dropdir"
    local staging="$dropdir/.bench-release.$.tmp"
    cat > "$staging" <<EOF
[Service]
ExecStart=
ExecStart=$exec_line --bench-release-gateway $2
EOF
    chmod 600 "$staging"
    mv -n "$staging" "$dropin"
    if [[ -e "$staging" ]]; then
      rm -f "$staging"
      echo "HATA: Dosya zaten var, uzerine yazilmadi." >&2
      return 1
    fi
    echo "SF5L0 BENCH test anahtari ile teslim onayi etkinlestirildi: $2"
  else
    if [[ ! -e "$dropin" ]]; then
      echo "Bench teslim modu zaten kapali."
      return
    fi
    rm -- "$dropin"
    echo "Bench teslim modu kaldirildi: yalnizca veri alma."
  fi

  systemctl --user daemon-reload
  systemctl --user restart orun-pc-edge.service
  systemctl --user status orun-pc-edge.service --no-pager -l || true
}

case "${1:-}" in
  install) shift; install_service "$@" ;;
  validate) shift; validate_install_args "$@" ;;
  bench-release) shift; bench_release "$@" ;;
  transport) shift; python3 "$ROOT/tools/ops/orun_pc_edge_transport.py" "$@" ;;
  status) systemctl --user status orun-pc-edge.service --no-pager || true ;;
  logs) journalctl --user -u orun-pc-edge.service -n 80 --no-pager ;;
  *) usage ;;
esac
