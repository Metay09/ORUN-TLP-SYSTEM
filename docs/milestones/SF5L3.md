# SF5L3 — bench map of the verified ledger

**Status:** code + host tests + headless-browser render check + **owner
phone check PASS (2026-10-24)**. Bench only (SF5L0 images, public test key).
**Base:** `main@efb377d` (SF5L0–L2 merged).
**Scope:** close the bench chain Tracker → Gateway → PC ledger → **map**.
Read-only; no firmware change.

## What it adds

- `tools/orun_edge_bench.py --serve <port> [--host <addr>]`: a small HTTP
  server next to the serial reader (or alone, over an existing ledger).
  - `GET /api/points`: PERIODIC records with a valid location, decoded from
    the SQLite ledger opened **read-only** per request.
  - `GET /`: the map page. Only a fixed whitelist of files is served.
  - Default bind `127.0.0.1`; `--host` exposes it (e.g. the Tailscale IP).
- `tools/edge_map/index.html`: Leaflet map, satellite (Esri) / street (OSM)
  layers, per-tracker track line, point markers fading by age, last-position
  marker, popup (record #, time, position, satellites/HDOP, battery, activity),
  tracker card and the 40 latest records; refreshes every 10 s; follows the
  newest position until the user drags. Phone layout uses a bottom sheet.
- Leaflet 1.9.4 vendored under `tools/edge_map/vendor/` (BSD-2, LICENSE
  included), so the page needs the internet only for map tiles.

## Evidence

- `test_sf5l3_edge_map.py`: 12 records sealed with the real SF5B scheme
  through the tool into a ledger; `/api/points` returns them decoded and in
  order; page and vendored assets served; traversal / non-whitelisted paths
  404; the ledger file is not modified by reads.
- Headless Chromium (desktop 1280×800, phone 390×844): 40 records render
  as track, markers, cards and list; popup opens; no page errors.

## Not in scope

Geofence zones, herd view, alarms, offline tiles, the ORUN app design of
2026-10-09, server/Coolify hosting, authentication.

## Owner check (2026-10-24, `73ce9ed`)

Tool on `simsek` with `--serve 8080 --host <Tailscale IP>`, page opened on
the owner's phone over Tailscale: satellite imagery, **278 located records**
of collar `0E8ADE7E71531AA3` (latest #11501, 7 satellites, HDOP 1.42,
4155 mV), track, popup, tracker card and latest-records list all live.

Observation: the collar is indoors and stationary, so the GNSS fixes scatter
in a ~10–15 m cloud and the connecting line draws a star. A stationary-aware
view (e.g. cluster/heat rather than a line for small moves) belongs to the
later ORUN app map, not this bench page.

