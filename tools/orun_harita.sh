#!/usr/bin/env bash
# ORUN 2. adım: internetsiz harita paketi (Aydın–Muğla, OpenStreetMap).
#
#   bash tools/orun_harita.sh
#
# Protomaps'in güncel OpenStreetMap haritasından yalnız Aydın–Muğla
# dikdörtgenini (z0–15) indirir, doğrular ve ORUN sunucusunun harita
# klasörüne koyar. Telefon uygulaması Ayarlar → "İnternetsiz harita"dan
# indirir. Sunucuyu yeniden başlatmak gerekmez.
# Ayda bir çalıştırmak yeter; paket güncelse hiçbir şey indirmez.
set -euo pipefail

REPO=$(cd "$(dirname "$0")/.." && pwd)
DATA="${XDG_DATA_HOME:-$HOME/.local/share}/orun"
MAPS="${ORUN_HARITA_DIR:-$DATA/maps}"
BIN="$DATA/bin"
PACK_ID=aydin-mugla
BBOX=27.0,36.3,29.9,38.3          # kaba dikdörtgen; il sınırı değil
MAXZOOM=15
PMTILES_VERSION=1.31.2
BUILDS=https://build-metadata.protomaps.dev/builds.json
SOURCE=https://build.protomaps.com

fail() { printf '\nHATA: %s\n' "$*" >&2; exit 1; }

case "$(uname -m)" in
  x86_64)        ARCH=x86_64; SUM=3ed7dbf4ec2e6dfe5e25b6f70d1ffc932729f93c86db353bf514dd71010a312f ;;
  aarch64|arm64) ARCH=arm64;  SUM=f8bd47e7ea866863489cad588fbaf2f31f42e5821f7a03f009b3769f05801cb1 ;;
  *) fail "Bu işlemci için pmtiles aracı yok: $(uname -m)" ;;
esac

mkdir -p "$MAPS" "$BIN"
TOOL="$BIN/pmtiles-$PMTILES_VERSION"
if [[ ! -x "$TOOL" ]]; then
  echo "== pmtiles aracı indiriliyor ($PMTILES_VERSION)"
  TMP=$(mktemp -d)
  trap 'rm -rf "$TMP"' EXIT
  TAR="go-pmtiles_${PMTILES_VERSION}_Linux_${ARCH}.tar.gz"
  curl -fsSL -o "$TMP/$TAR" \
    "https://github.com/protomaps/go-pmtiles/releases/download/v$PMTILES_VERSION/$TAR"
  echo "$SUM  $TMP/$TAR" | sha256sum -c --quiet - || fail "pmtiles aracının parmak izi tutmadı."
  tar -xzf "$TMP/$TAR" -C "$TMP" pmtiles
  install -m 0755 "$TMP/pmtiles" "$TOOL"
fi

echo "== Güncel harita sürümü soruluyor"
KEY=$(curl -fsS "$BUILDS" | python3 -c '
import json, re, sys
keys = [b.get("key", "") for b in json.load(sys.stdin)]
keys = [k for k in keys if re.fullmatch(r"[0-9]{8}\.pmtiles", k)]
print(max(keys) if keys else "")
') || fail "Harita sürüm listesi alınamadı (internet?)."
[[ -n "$KEY" ]] || fail "Harita sürüm listesi boş geldi."
VERSION=${KEY%.pmtiles}
NAME="$PACK_ID-$VERSION.pmtiles"

if [[ -f "$MAPS/$NAME" && -f "$MAPS/$NAME.json" ]]; then
  echo "Harita güncel: $NAME ($(du -h "$MAPS/$NAME" | cut -f1))"
  exit 0
fi

PART="$MAPS/.$NAME.part"
rm -f "$PART"
echo "== Aydın–Muğla haritası indiriliyor ($VERSION, z0–$MAXZOOM; birkaç dakika sürebilir)"
"$TOOL" extract "$SOURCE/$KEY" "$PART" --bbox="$BBOX" --maxzoom="$MAXZOOM" \
  || { rm -f "$PART"; fail "Harita indirilemedi."; }
"$TOOL" verify "$PART" >/dev/null || { rm -f "$PART"; fail "İnen harita dosyası bozuk."; }
mv -f "$PART" "$MAPS/$NAME"

python3 "$REPO/tools/maps/make_offline_catalog.py" "$MAPS/$NAME" --sidecar \
  --id "$PACK_ID" --version "$VERSION" --title "Aydın–Muğla harita" \
  --kind protomaps-v4-vector --bounds "$BBOX" --minzoom 0 --maxzoom "$MAXZOOM" \
  --attribution "© OpenStreetMap katkıcıları · Protomaps" \
  --license "ODbL Produced Work" >/dev/null \
  || { rm -f "$MAPS/$NAME"; fail "Harita paketi bilgisi yazılamadı."; }

# Eski sürümler: yenisi hazır olduktan sonra silinir.
for old in "$MAPS/$PACK_ID"-*.pmtiles; do
  [[ "$old" == "$MAPS/$NAME" ]] && continue
  rm -f "$old.json" "$old"
done

echo "Tamam: $NAME hazır ($(du -h "$MAPS/$NAME" | cut -f1)). Telefonda Ayarlar → İnternetsiz harita → İndir."
