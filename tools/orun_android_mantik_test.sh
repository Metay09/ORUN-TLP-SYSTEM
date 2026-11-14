#!/usr/bin/env bash
# ORUN: Android izleme servisinin tek kuralını (hangi uyarı bir kez gösterilir)
# PC'de derleyip dener. Android SDK gerekmez; yalnız JDK (javac, java).
#
#   bash tools/orun_android_mantik_test.sh
set -euo pipefail
REPO=$(cd "$(dirname "$0")/.." && pwd)
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT
javac -nowarn -d "$OUT" \
  "$REPO/edge/app/android/app/src/main/java/tr/orun/edge/HerdWatchLogic.java" \
  "$REPO/tools/android_mantik/HerdWatchLogicCheck.java"
java -cp "$OUT" tr.orun.edge.HerdWatchLogicCheck
