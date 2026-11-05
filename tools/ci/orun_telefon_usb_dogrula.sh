#!/usr/bin/env bash
# 4c: Safe Android compile check on an ORUN development PC.
# No USB port, flash operation, credential change, or APK distribution.
# Usage: bash tools/ci/orun_telefon_usb_dogrula.sh
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
APP="$ROOT/edge/app"
fail() { printf 'DUR: %s\n' "$*" >&2; exit 2; }
step() { printf '\n=== %s ===\n' "$*"; }

[[ -f "$APP/package-lock.json" && -d "$APP/android" ]] ||
  fail 'edge/app/package-lock.json veya Android projesi bulunamadi.'
command -v node >/dev/null || fail 'Node.js eksik (22 veya daha yeni gerekli).'
command -v npm >/dev/null || fail 'npm eksik.'
command -v java >/dev/null || fail 'Java (JDK 21) eksik.'
command -v python3 >/dev/null || fail 'Python 3 eksik.'

step '1/5 Firmware paketi Python testleri'
(cd "$ROOT" && python3 -m unittest discover -s tools/ci -p test_orun_telefon_paketi.py -v)

step '2/5 Kilitli npm bagimliliklari'
(cd "$APP" && npm ci)

step '3/5 Uygulamanin tum Node testleri'
(cd "$APP" && npm test)

step '4/5 Tam TypeScript + Vite yapisi, Android esitleme'
(cd "$APP" && VITE_ORUN_USB_FLASH_EXPERIMENTAL=0 npm run android)

step '5/5 Android debug APK derleme (cihaza yuklemez)'
(cd "$APP/android" && bash ./gradlew :app:assembleDebug --no-daemon --stacktrace)

APK="$APP/android/app/build/outputs/apk/debug/app-debug.apk"
[[ -s "$APK" ]] || fail 'Gradle bitti ancak debug APK bulunamadi.'
printf '\n=== ANDROID DEBUG DERLEME PASS ===\n'
printf 'APK: %s\n' "$APK"
if command -v sha256sum >/dev/null; then sha256sum "$APK"; fi
printf 'Dikkat: Bu yalnız derlemedir; Android-RAK USB flash ve kayit koruma testi YAPILMADI.\n'
