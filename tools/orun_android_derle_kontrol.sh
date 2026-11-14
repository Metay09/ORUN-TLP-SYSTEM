#!/usr/bin/env bash
# Telefon uygulamasının bütün Java kodunu gerçek Android API'sine (android.jar)
# karşı derler: Gradle/APK olmadan, yazım ve API hatalarını önceden yakalar.
# Capacitor sınıfları tools/android_mantik/capacitor_stub altındaki imza
# kopyalarından gelir. APK'nın kendisi yine Gradle ile yapılır.
#   ANDROID_JAR=/yol/android-36/android.jar bash tools/orun_android_derle_kontrol.sh
set -euo pipefail
REPO="$(cd "$(dirname "$0")/.." && pwd)"
JAR="${ANDROID_JAR:-${ANDROID_HOME:-$HOME/Android/Sdk}/platforms/android-36/android.jar}"
[ -f "$JAR" ] || { echo "android.jar yok: ANDROID_JAR=... ver" >&2; exit 2; }
OUT="$(mktemp -d)"; trap 'rm -rf "$OUT"' EXIT
# R sınıfı: res/ klasöründen kaynak adları (Gradle'ın ürettiğinin imza kopyası).
python3 - "$REPO/edge/app/android/app/src/main/res" "$OUT/gen" <<'PY'
import os, re, sys
res, out = sys.argv[1], sys.argv[2]
kinds = {}
for d in sorted(os.listdir(res)):
    kind = d.split("-")[0]
    for f in sorted(os.listdir(os.path.join(res, d))):
        if kind == "values":
            text = open(os.path.join(res, d, f), encoding="utf-8").read()
            for tag, name in re.findall(r'<(string|color|style|dimen|bool|integer)\s+name="([^"]+)"', text):
                kinds.setdefault(tag, set()).add(name.replace(".", "_"))
        else:
            kinds.setdefault(kind, set()).add(f.split(".")[0])
os.makedirs(os.path.join(out, "tr/orun/edge"), exist_ok=True)
with open(os.path.join(out, "tr/orun/edge/R.java"), "w") as w:
    w.write("package tr.orun.edge;\npublic final class R {\n")
    for kind, names in sorted(kinds.items()):
        w.write(f"  public static final class {kind} {{\n")
        for i, n in enumerate(sorted(names)):
            w.write(f"    public static final int {n} = 0x7f{abs(hash(kind)) % 255:02x}{i:04x};\n")
        w.write("  }\n")
    w.write("}\n")
PY
mapfile -t SRC < <(find "$OUT/gen" "$REPO/edge/app/android/app/src/main/java" "$REPO/tools/android_mantik/capacitor_stub" -name '*.java' | sort)
javac -Xlint:deprecation -Xlint:unchecked --release 17 -nowarn -cp "$JAR" -d "$OUT" "${SRC[@]}" 2>&1 | grep -v '^Picked up' || true
test -f "$OUT/tr/orun/edge/MainActivity.class" && echo "DERLEME PASS (${#SRC[@]} dosya, $(basename "$(dirname "$JAR")"))" || { echo "DERLEME FAIL"; exit 1; }
