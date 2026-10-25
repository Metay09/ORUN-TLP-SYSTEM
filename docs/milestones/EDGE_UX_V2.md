# Edge app UX v2 — map first, calm, plain Turkish

**Status:** code + host tests + screenshot review with sample data. **On the phone: pending (owner).**
**Scope:** UI only (`edge/app/src/ui`). Nothing changed in the core:
- ledger, hand-off, upload and keys are unchanged;
- BLE/USB transport is unchanged;
- the `edge/app/src/core` tests are unchanged.

## Why

The owner found the SF5L7 screen and the ChatGPT prototype (#173) "sert" (harsh). The causes:
- a large header and uppercase labels;
- boxes inside boxes;
- engineering words;
- number tiles;
- a small or secondary map;
- emoji;
- five tabs.

The redesign follows what established livestock and field-map apps do. Halter, Nofence, Digitanimal, Vence and eShepherd are map first. Gaia and onX are the field-map references. Material 3 sheets and navigation are the platform guidance.

## What the owner sees

- **Harita (home)**
  - Full-screen map with round pins in each animal's colour.
    - A named animal shows its initials.
    - An unnamed collar shows a cow glyph.
    - Pin ring: amber means aging, grey means old position.
  - Status pill, top left: "İstasyona bağlı" plus a cloud mark for the server. Tapping it opens *İstasyon ve sunucu*.
  - Three round buttons, top right:
    - map type: Uydu / Harita / Arazi;
    - my position;
    - show the whole herd.
  - Bottom sheet with three heights. Drag it, or tap it to cycle.
    - Collapsed it shows one sentence, e.g. "4 hayvan · 2 tanesine bakın".
    - Open it shows the herd, attention first: critical battery → no news → low battery → waiting for a first fix → fine.
  - Tapping a pin or a row turns the sheet into the **animal card** without leaving the map:
    - status sentence ("8 dk önce görüldü" / "35 dakikadır haber yok");
    - "Bana uzaklığını göster" → "420 m · kuzeydoğu", with a rotated arrow;
    - movement now;
    - battery ("Pil iyi · %87");
    - the last 24 h trail on the map;
    - *Yol tarifi* opens the phone's maps app (`geo:`);
    - *Adını değiştir*: name, colour, note.
- **Sürü:** the same herd list on its own page, and *Yeni tasma*. For now *Yeni tasma* explains that collars are set up from the PC; the USB-OTG flow on the phone comes with 3C.
- **Defter:**
  - a sync summary card, e.g. "4 kayıt telefonda saklı · internet gelince gönderilecek" or "Tüm kayıtlar gönderildi";
  - a day-grouped timeline: station connected/lost, new records, hand-offs, uploads, read keys, server unreachable.
- **İstasyon ve sunucu** (from the pill or the Defter card):
  - Bluetooth / USB / another station / disconnect;
  - server address, key, automatic upload, "Şimdi gönder";
  - *Teknik ayrıntılar*, collapsed: station id, held, counters, storage and the raw log.
- **Android back:** closes the layer menu → the modal → the animal card → returns to Harita from another tab → collapses the sheet → sends the app to the background.

## Rules kept (and tested)

- **Freshness** is the collar's RECORD time, never the arrival time. A record measured 2 h ago that reaches the phone now reads "2 saattir haber yok".
- **Thresholds** come from the collar's period T: fresh ≤ max(2T, 10 min), aging ≤ max(6T, 60 min), then old. An old animal keeps its last position, greyed.
- **Red** only for critical battery. Staleness is amber or grey, never alarm red.
- **Battery:**
  - the percentage is estimated from the cell voltage (3.50–4.15 V);
  - the collar's own LOW/CRITICAL state wins;
  - no "days left" yet, because there is no drain model.
- **Turkish wording:**
  - fixed suffixes ("dakikadır", "saattir", "gündür");
  - "tanesi" after numbers, so no suffix-harmony errors;
  - sentence case;
  - Figtree with its latin-ext subset (ş ğ İ ı).

## Pieces

- `src/ui/herd.ts`: the pure view model (herd, wording, battery, distance, direction). Tested by `test/herd.test.ts`.
- `src/ui/App.tsx`, `MapView.tsx` (Leaflet 1.9.4 from npm), `Sheet.tsx`, `profiles.ts` (names, colours and notes, kept on the phone) and `styles.css`:
  - warm paper `#F7F4EC` with pasture green `#3E6B3A`;
  - dark theme follows the phone;
  - 52–56 px buttons.
- `controller.ts`:
  - `points()` for the views;
  - `pointsVersion`;
  - `journal` (the Defter lines), kept in local storage (300 lines). Bursts are merged into one line ("38 yeni kayıt …").
- `AndroidManifest.xml`: coarse/fine location. Asked only when the owner taps "Konumum" or "Bana uzaklığını göster". BLE scanning stays `neverForLocation`.
- `preview.html` + `src/preview.tsx` (dev only, not in the app build): the UI with sample data, for screenshots (`npm run dev` → `/preview.html`).

## Known limits / next

- **Map imagery** (Esri satellite, OSM, OpenTopoMap) is online only, as before. The animals' positions show without internet on a plain background.
  - Offline map packages are later work: PMTiles built from openly licensed data (OSM extract, Sentinel-2 cloudless 2016 CC BY 4.0).
  - Offline export from OSM's and Esri's public servers is not allowed, so it will not be built on them.
- **Not yet:**
  - virtual fences (*Alan dışı* alerts);
  - battery days-left;
  - elevation difference to the animal (the collar's altitude is not in the map points yet);
  - names shared between phones (server side, with 3C accounts).
- **Yeni tasma** on the phone (USB-OTG, sealed credential envelope) is 3C.
