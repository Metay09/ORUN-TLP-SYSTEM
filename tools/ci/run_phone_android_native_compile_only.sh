#!/usr/bin/env bash
# ORUN S5D: compile Android native Java and Capacitor bridge WITHOUT building
# or installing an APK. No service/Tailscale/Gateway/Tracker operations.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"

echo "=== ORUN S5D ANDROID NATIVE COMPILE PRECHECK ==="
test -f edge/app/android/gradlew
test -f edge/app/android/app/src/main/java/tr/orun/edge/OrunMobileIdentityPlugin.java
test -f edge/app/android/app/src/main/java/tr/orun/edge/OrunMobileSessionPlugin.java
test -f edge/app/android/app/src/main/java/tr/orun/edge/MainActivity.java

# Never auto-install dependencies / regenerate Android project / assemble APK.
if [ ! -d edge/app/node_modules/@capacitor/android ]; then
  echo "ORUN_ANDROID_NATIVE_COMPILE_NOT_RUN=CAPACITOR_DEPENDENCIES_MISSING"
  exit 3
fi
if ! command -v java >/dev/null 2>&1; then
  echo "ORUN_ANDROID_NATIVE_COMPILE_NOT_RUN=JAVA_MISSING"
  exit 3
fi
if ! command -v javac >/dev/null 2>&1; then
  echo "ORUN_ANDROID_NATIVE_COMPILE_NOT_RUN=JAVAC_MISSING"
  exit 3
fi
if [ -n "${ANDROID_HOME:-}" ]; then
  test -d "$ANDROID_HOME" || {
    echo "ORUN_ANDROID_NATIVE_COMPILE_NOT_RUN=ANDROID_HOME_INVALID"
    exit 3
  }
fi

echo "=== ORUN ANDROID JAVA-ONLY TASK (NO APK / NO INSTALL) ==="
cd edge/app/android
# --offline does not resolve/download new Gradle dependencies. If caches are
# absent, stop clearly; never silently initiate a full Android build.
bash ./gradlew --offline --no-daemon --max-workers=2 \
  :app:compileDebugJavaWithJavac
echo "ORUN_ANDROID_NATIVE_JAVA_COMPILE_PASS"
