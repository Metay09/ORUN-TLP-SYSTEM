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
  "$SRC/BridgeSpool.java" "$SRC/BridgeCore.java" "$REPO/tools/android_mantik/BridgeCoreCheck.java" \
  "$SRC/Sql.java" "$SRC/BridgeStore.java" "$REPO/tools/android_mantik/PySql.java" \
  "$SRC/BridgeLink.java" "$REPO/tools/android_mantik/BridgeLinkCheck.java" \
  "$SRC/BridgeUpload.java" "$REPO/tools/android_mantik/BridgeUploadCheck.java" \
  "$REPO/tools/android_mantik/HerdWatchLogicCheck.java" \
  "$REPO/tools/android_mantik/BridgeWireCheck.java"
java -cp "$OUT" tr.orun.edge.HerdWatchLogicCheck
# Yerel köprü: Java kopyası, TypeScript aslının ürettiği vektörlerle aynı mı.
java -cp "$OUT" tr.orun.edge.BridgeWireCheck "$REPO/tools/android_mantik/bridge_vectors.txt"
# Yerel köprü çekirdeği: uygulamanın gerçek EdgeCore'uyla oynatılmış senaryolarda adım adım aynı mı.
java -cp "$OUT" tr.orun.edge.BridgeCoreCheck "$REPO/tools/android_mantik/bridge_core_vectors.txt"
# Yerel köprünün tamamı: benzetilmiş gateway, kopmalar, Bluetooth kapanması, menzil dışı, yazma hataları.
java -cp "$OUT" tr.orun.edge.BridgeLinkCheck "$REPO/tools/android_mantik/bridge_pool.txt" 12
# Yerel köprünün sunucuya gönderişi: sunucunun gerçek toplu kayıt koduna karşı.
java -cp "$OUT" tr.orun.edge.BridgeUploadCheck "$REPO/tools/android_mantik/bridge_pool.txt" "$REPO"
