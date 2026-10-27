#!/usr/bin/env bash
# ORUN BENCH backend: opt-in persistent Debian systemd user service.
# Does not flash Gateway, mutate credentials, erase records, or alter Edge.
set -euo pipefail
umask 077

SELF="$(readlink -f "${BASH_SOURCE[0]}")"
ROOT="$(cd "$(dirname "$SELF")/../.." && pwd)"
TOKEN_FILE="$HOME/.config/orun/backend.token"
DB_FILE="$HOME/orun-backend-bench.sqlite"
REGISTRY_FILE="$HOME/orun-credentials.sqlite"
UNIT_DIR="$HOME/.config/systemd/user"
UNIT_FILE="$UNIT_DIR/orun-backend.service"
PORT=8090

tailscale_host() {
  if ! command -v tailscale >/dev/null 2>&1; then
    echo "HATA: Tailscale komutu bulunamadi. Bu servis sadece ozel Tailscale aginda dinler." >&2
    return 1
  fi
  local address
  address="$(tailscale ip -4 2>/dev/null | head -n1)"
  if [[ ! "$address" =~ ^[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
    echo "HATA: Tailscale IPv4 henuz hazir degil. Systemd sonra yeniden dener." >&2
    return 1
  fi
  printf '%s' "$address"
}

require_token() {
  if [[ ! -s "$TOKEN_FILE" ]]; then
    echo "HATA: $TOKEN_FILE eksik/bos. Once 'install' yapin." >&2
    return 1
  fi
}

install_service() {
  if [[ "$ROOT" == *" "* || "$SELF" == *" "* ]]; then
    echo "HATA: Proje yolunda bosluk var; systemd komutu olusturulmadi." >&2
    exit 1
  fi
  command -v systemctl >/dev/null || { echo "HATA: systemd gerekli." >&2; exit 1; }
  python3 -c 'import cryptography' || {
    echo "HATA: cryptography yok. Debian'da python3-cryptography paketini kurun." >&2
    exit 1
  }
  tailscale_host >/dev/null
  # A missing credential registry is NOT a blank first-run setup. The
  # backend would otherwise create an empty key database and reject all
  # actual Tracker data. Refuse rather than masking a wrong machine/path.
  if [[ ! -s "$REGISTRY_FILE" ]]; then
    echo "HATA: Gercek tasma anahtar kaydi bulunamadi: $REGISTRY_FILE" >&2
    echo "Bu PC gercek ORUN backend makinesi degilse anahtar tasimadan servisi kurmayin." >&2
    echo "Mevcut registry yolunu dogrulayin; sifir anahtar olusturmayin." >&2
    exit 1
  fi
  mkdir -p "$UNIT_DIR" "$(dirname "$TOKEN_FILE")"
  chmod 700 "$(dirname "$TOKEN_FILE")"

  if [[ ! -s "$TOKEN_FILE" ]]; then
    if [[ ! -t 0 ]]; then
      echo "HATA: Mevcut Edge tokenini guvenli girmek icin etkilesimli terminal gerekir." >&2
      exit 1
    fi
    local token
    IFS= read -r -s -p "ORUN Edge'de Ayarlar > Sunucu'da girili mevcut token: " token
    printf '\n'
    if (( ${#token} < 8 )) || [[ "$token" == *$'\n'* || "$token" == *$'\r'* ]]; then
      echo "HATA: En az 8 karakter, tek satirlik token gerekli." >&2
      exit 1
    fi
    printf '%s\n' "$token" > "$TOKEN_FILE"
    unset token
  fi
  chmod 600 "$TOKEN_FILE"

  cat > "$UNIT_FILE" <<EOF
[Unit]
Description=ORUN Backend (bench, private Tailscale listener)
Documentation=https://github.com/Metay09/ORUN-TLP-SYSTEM
# A user unit retries until Tailscale gets its IPv4; no public bind.

[Service]
Type=simple
WorkingDirectory=$ROOT
ExecStart=/usr/bin/bash $SELF serve
Restart=always
RestartSec=10
UMask=0077
NoNewPrivileges=true

[Install]
WantedBy=default.target
EOF
  systemctl --user daemon-reload
  systemctl --user enable --now orun-backend.service
  echo "=== ORUN BACKEND SERVIS ==="
  systemctl --user is-active orun-backend.service || true
  echo "Servis: $UNIT_FILE"
  echo "DB (mevcut dosya korunur): $DB_FILE"
  echo "Registry (mevcut dosya korunur): $REGISTRY_FILE"
  local host
  host="$(tailscale_host)"
  echo "Edge Sunucu URL: http://$host:$PORT"
  if command -v curl >/dev/null 2>&1; then
    if ! curl --connect-timeout 3 --max-time 5 -fsS "http://$host:$PORT/v1/health"; then
      echo "UYARI: Health henuz cevap vermiyor. 'status' ve 'logs' ile inceleyin." >&2
    fi
    printf '\n'
  fi
  if command -v loginctl >/dev/null 2>&1; then
    if ! loginctl show-user "$(id -un)" -p Linger --value 2>/dev/null | grep -qx yes; then
      echo "DIKKAT: Oturum acmadan yeniden baslamasi icin bir kez calistirin:" >&2
      echo "  sudo loginctl enable-linger $(id -un)" >&2
    fi
  fi
}

case "${1:-}" in
  install) install_service ;;
  serve)
    require_token
    HOST="$(tailscale_host)"
    exec python3 "$ROOT/backend/orun_backend_bench.py" \
      --db "$DB_FILE" --registry "$REGISTRY_FILE" \
      --token-file "$TOKEN_FILE" --host "$HOST" --port "$PORT"
    ;;
  status)
    systemctl --user status orun-backend.service --no-pager || true
    if HOST="$(tailscale_host)"; then
      curl --connect-timeout 3 --max-time 5 -fsS "http://$HOST:$PORT/v1/health" || true
      printf '\n'
    fi
    ;;
  logs)
    journalctl --user -u orun-backend.service --no-pager -n 60
    ;;
  *)
    echo "Kullanim: bash tools/ops/orun_backend_service.sh {install|status|logs}" >&2
    exit 2
    ;;
esac
