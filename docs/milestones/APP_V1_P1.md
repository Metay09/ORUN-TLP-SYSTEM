# ORUN app v1 — package 1: skeleton, design system, field map

**Status:** code + host tests + browser screenshot review (sample herd, no map
tiles in the sandbox). **On the phone: pending (owner).**
**Scope:** package 1 of `docs/product/ORUN_UYGULAMA_KAPSAMI.md` §15.
Supersedes the unmerged UX v2 (#174).

## What changed

- **Architecture (§14).** `src/app` holds the domain model (Device, Asset,
  Assignment with a validity start, Observation, Station, AppEvent), the
  data-source seam (`EdgeSource`: phone ledger `Controller` | sample herd
  `DemoSource`), pure derivations and small persisted stores. `src/ds` is the
  design system, `src/features/*` the screens. One dictionary:
  `src/app/i18n/tr.ts`. `src/core` is unchanged except one decoded field.
- **Navigation:** five tabs (Harita · Sürü · Alanlar · Olaylar · Ağ), pushed
  screens (animal, device, Bul, settings) and one back-key stack (dialog →
  screen → map selection → map tab → background).
- **Map:** MapLibre GL 6 instead of Leaflet.
  - Bases: Esri satellite + place names, OpenTopoMap, OpenFreeMap vector.
  - 3D relief from AWS Terrain Tiles, optional hillshade.
  - Freshness-ring pins, clustering, a pulse for alarm.
  - Selected animal: trail for today / 24 h / 3 / 7 days, gaps dashed, time
    slider and playback; a single day can be replayed from the profile.
  - Layers: density (heatmap), 5 m coverage, station, phone position.
- **Bul:** distance, arrow by compass (or north-up when the phone has none),
  height difference from the two GNSS heights, age of the position, directions.
- **Sürü / profile:** search, filters, today's herd summary; profile tabs
  Bugün (position with measured/arrived times, hourly movement strip, distance,
  battery curve), Geçmiş (per day + events), Bilgiler (identity, dated notebook,
  collar).
- **Olaylar:** active first with "Gördüm", then the day-grouped book.
- **Ağ:** "Kayıt güvende mi?" chain, station link and memory, station position
  (marked from the phone), devices with services, coverage summary, server,
  technical details and log.
- **Ayarlar:** theme (auto / light / dark), sun mode, sample herd, about.
- **Tanıtım sürüsü:** 12 animals, 7 days, 15 min reports, generated on the phone
  from the same record shape; marked "Tanıtım sürüsü" on every screen. Always on
  in a browser (no ledger there).
- **Decoded altitude:** `alt_m` (WGS84 ellipsoid height) added to the periodic
  decode in `core/product.ts` and `tools/orun_edge_bench.py`, and to map points,
  so the phone and backend maps stay equal.

## Rules kept (tested)

- "Where is it now" = newest **measurement** time; late records only extend
  history. Freshness from period T: fresh ≤ max(2T, 10 min), aging ≤ max(6T, 60 min).
- Silent > max(6T, 60 min); "kayıp olabilir" > max(12T, 3 h) (reports missed).
- A gap that later arrivals fill is not an event.
- Battery: the worst level of the last 24 h (solar dips at night); an episode
  closes only after a full day back at good.
- Coverage: only *live* records are signal samples for their position; a
  cell with only *cut* records is "no link"; empty cells are not drawn.
- No grazing/resting/walking classes and no battery-days estimate: the collars
  do not send them yet (packages 8 and later).

## Tests

- `npm test` (31): core (unchanged), herd, wording, battery, assignment with
  validity, events, coverage, activity, sample-herd determinism.
- `npx tsc --noEmit`, `npx vite build`.
- Python: SF5L1, SF5L3, SF5L4, SF5L7 (app core = Python reference), SF5L8
  (backend map == phone map).

## Fix after the first phone install (2026-10-26)

- The station's waiting-record count stayed at its connection value: the
  Gateway reports it only in HELLO. `EdgeCore.heldNow()` now follows it live:
  the HELLO count minus confirmed hand-offs until the first DUMP END, then
  exactly the records the Gateway announced minus those handed off or on a
  reclaimed page; the last value is kept after disconnect. Test in
  `test/core.test.ts`. No firmware change.

## Station counter regression guard (2026-10-26)

- BLE `DUMP END` and USB `EDGE DUMP END count=N` are checked against distinct
  announced `page:slot` records, including records already handed off.
  Missing/broken announcements no longer turn a partial scan into an exact,
  misleadingly low Gateway count; the app logs that a fresh DUMP is needed.
- A requested BLE/USB redump starts a fresh announcement census. Live records
  already handed off do not cause false mismatch warnings on the next pass.
- Three focused regression tests have been added in `edge/app/test/core.test.ts`.
  They still require an actual `npm test` and Android check on this PR head.

## Map, Alanlar and trails (2026-10-27)

Carried over from the field prototype (`ORUN-Saha.html`), into the current app:

- **Satellite zoom fix.** Esri has no 18-19 imagery over rural Aydın and
  answers with a "Map data not yet available" picture (HTTP 200, not an
  error). Tiles are now asked up to a settable limit (default 17, 14-19 in
  the layer panel) and enlarged beyond. Esri Wayback releases can be chosen.
  Raster tiles switch without a fade; metre scale; a chip when base tiles
  keep failing.
- **Pins.** One size rule by zoom (dot far out, half size, full pin close in),
  continuous during the zoom; touch target stays 44 px. No CSS drop-shadow
  filters (hairline edge instead). Alarm and selected pins never join a
  cluster. Data sources are re-sent only when their content changed.
- **Alanlar.** `src/app/zones.ts`: E7 validation and inside/boundary/outside
  identical to `firmware/src/geofence_geometry.cpp` (20 000 random cases
  compared against the compiled firmware source: 0 differences), 8 areas /
  64 corners, simplify, GeoJSON in/out. Zones are stored on the phone
  (`orun.app.zones.v1`), drawn on the map (fill none/light/hatched, names,
  edge lengths on the selected one), drawn and edited on the map (cross-hair
  or taps, drag corners, insert at edge middles, undo, phone position as a
  corner, live ha/dekar/corners/perimeter, the collar's rules, "Sadeleştir").
- **Phone-side "alan dışında".** The phone compares the records it holds with
  the active zones, using the policy's false-alarm rule (newest record outside
  and 2 of the last 3 accepted records outside; poor fixes ignored). This is
  an interim phone view: zones are **not** written to collars yet (the USB
  `APP FENCE` writer exists; the app does not drive it), so the collar does
  not raise OUTSIDE or change cadence. When collar OUTSIDE events reach the
  phone, they replace this derivation (AGENTS: no second majority layer).
- **Serbest otlatma** 2 / 6 / 12 h or until turned off: outside periods raise
  nothing; lost, silent and battery warnings continue. Chip on the map.
- **Trails.** Whole-herd trails layer, trail points darkening old → new,
  animal → station link lines coloured by the last record's RSSI.
- Events: "Bul" and "Haritada göster" on active animal warnings. Ağ:
  "Bağlantıyı kes" moved to the bottom of the station card.
- Tests: `test/zones.test.ts`.

## Pending on the phone

- Map tiles through Capacitor (satellite, topo, vector, terrain); whether 17
  is the right satellite limit over the pasture (lower it if the placeholder
  text still shows).
- Zone drawing by finger (drag, cross-hair) and GeoJSON import via the file
  picker; export copies to the clipboard on Android.
- Compass heading (`deviceorientationabsolute`) and GNSS height on the device.
- BLE/USB link, hand-off and upload unchanged; check once with a real Gateway.
