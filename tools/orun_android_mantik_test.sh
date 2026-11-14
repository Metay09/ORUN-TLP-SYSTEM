#!/usr/bin/env bash
# ORUN: Android tarafının SDK'sız denetimi: kaynak XML'leri iyi biçimli mi,
# ve izleme servisinin tek kuralı (hangi uyarı bir kez gösterilir).
# Android SDK gerekmez; yalnız python3 ve JDK (javac, java).
#
#   bash tools/orun_android_mantik_test.sh
set -euo pipefail
REPO=$(cd "$(dirname "$0")/.." && pwd)
python3 "$REPO/tools/android_mantik/xml_check.py" "$REPO/edge/app/android/app/src/main"
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT
SRC="$REPO/edge/app/android/app/src/main/java/tr/orun/edge"
javac -nowarn -d "$OUT" \
  "$SRC/HerdWatchLogic.java" "$SRC/EdgeWire.java" "$SRC/ProductFrame.java" \
  "$REPO/tools/android_mantik/HerdWatchLogicCheck.java" \
  "$REPO/tools/android_mantik/BridgeWireCheck.java"
java -cp "$OUT" tr.orun.edge.HerdWatchLogicCheck
# Yerel köprü: Java kopyası, TypeScript aslının ürettiği vektörlerle aynı mı.
java -cp "$OUT" tr.orun.edge.BridgeWireCheck "$REPO/tools/android_mantik/bridge_vectors.txt"
