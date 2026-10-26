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

## Pending on the phone

- Map tiles through Capacitor (satellite, topo, vector, terrain).
- Compass heading (`deviceorientationabsolute`) and GNSS height on the device.
- BLE/USB link, hand-off and upload unchanged; check once with a real Gateway.
