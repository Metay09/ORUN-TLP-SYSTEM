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
javac -nowarn -d "$OUT" \
  "$REPO/edge/app/android/app/src/main/java/tr/orun/edge/HerdWatchLogic.java" \
  "$REPO/tools/android_mantik/HerdWatchLogicCheck.java"
java -cp "$OUT" tr.orun.edge.HerdWatchLogicCheck
