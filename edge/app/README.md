# ORUN Edge (Android app, bench)

The phone as the Gateway's Edge: reads the Gateway over Bluetooth LE or USB
OTG, stores every verified record in the phone's SQLite ledger, then
confirms the hand-off so the Gateway can free the space. Shows the field map.
Design and evidence: `docs/milestones/SF5L7.md`.

**Bench only:** uses the public SF5L0 test key.

## Requirements (build machine)

- Node.js 22 or newer
- JDK 21
- Android SDK with platform 36 (Android Studio installs it), `ANDROID_HOME` set

## Build and install

```sh
cd edge/app
npm ci
npm run android          # web build + copy into android/
cd android
./gradlew assembleDebug  # -> app/build/outputs/apk/debug/app-debug.apk
adb install -r app/build/outputs/apk/debug/app-debug.apk
```

## Develop

- `npm test`: core unit tests (Node, no phone)
- `npm run typecheck`
- `python3 firmware/tests/sf5/test_sf5l7_edge_app.py` (from the repo root):
  the core against the Python reference

The map page is `tools/edge_map/index.html`, copied into `public/` by
`npm run map` (part of `dev`/`build`); edit it there, not in `public/`.
