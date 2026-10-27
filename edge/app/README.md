# ORUN app (Android, Capacitor)

The owner's phone app: field map, herd, events and the network. The phone is
also the Gateway's Edge: it reads the Gateway over Bluetooth LE or USB OTG,
stores every verified record in the phone's SQLite ledger, confirms the
hand-off so the Gateway can free the space, and uploads to the server.

- Full scope (Turkish): `docs/product/ORUN_UYGULAMA_KAPSAMI.md`
- Package 1 status: `docs/milestones/APP_V1_P1.md`
- Core (ledger, hand-off, upload, keys): `docs/milestones/SF5L7.md`, `SF5L8.md`

**Bench security:** the public SF5L0 test key is accepted only by the bench build.

## Layout

```
src/core/       ledger, product frames, hand-off, upload, read keys (unchanged by the UI)
src/platform/   Capacitor SQLite, BLE / USB transports, HTTP, foreground service
src/app/        domain model, data sources (phone ledger | sample herd), derived
                herd / devices / events / coverage / activity, stores, nav, i18n/tr.ts
src/ds/         design system "ORUN Saha": tokens (light / dark / sun) + components
src/features/   map (MapLibre engine, sheet, layers, Bul), herd, zones, events,
                network, settings
```

Screens read one model (`useApp()`); they never see where the data came from.
Every visible text lives in `src/app/i18n/tr.ts`.

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

- `npm run dev`: the app in a browser. Without the phone there is no ledger,
  so it always shows the sample herd ("Tanıtım sürüsü").
- `npm test`: unit tests (core, herd, events, coverage, activity, sample herd,
  zones = the collar's geofence geometry)
- `npm run typecheck`
- `python3 firmware/tests/sf5/test_sf5l7_edge_app.py` (from the repo root):
  the core against the Python reference

## Map sources

No account or key: Esri World Imagery or an Esri Wayback release (satellite,
attribution shown; asked up to the layer panel's tile limit, default 17, and
enlarged beyond, because Esri answers missing 18-19 imagery with a "Map data
not yet available" picture),
OpenTopoMap (terrain), OpenFreeMap "liberty" (street, vector), AWS Terrain
Tiles / Terrarium (3D relief). Without internet the base is blank but
animals, trails and layers still draw. Offline packages come with package 3.
