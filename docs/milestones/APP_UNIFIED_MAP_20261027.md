# ORUN Android — Unified map (PR #180 + Claude PR #183)

Date: 2026-10-27. **Status: code assembled; host tests and Android
smoke test required. Not a confirmed production release.**

## Branch strategy
Branch `feat/app-map-unified-no-phone-outside-alarm` starts from
`app/harita-alanlar` (Claude PR #183). The earlier independent PR #180
remains untouched; this branch ports its safety/interaction features without
trying to auto-merge two different ZonesScreen/state architectures.
Nothing has been merged into `main`.

## Preserved from Claude #183
- Configurable Esri imagery tile cap (z14–19, default z17), Esri Wayback
  choices, continuous pin scale, new drawing overlay and zone detail/list
  screens, station link lines, trails/coverage and layer visibility.
- E7 geofence validation, field-scale WGS84 GeoJSON, polyline length/area
  previews and phone GPS vertex option.
- Existing firmware gateway hand-off, ACK, BLE, USB, SQLite and backend
  interfaces are not changed.

## Ported or reconciled from earlier #180
- Three-level `Sheet.tsx` with pointer capture on the real grip, 28px
  deliberate drag/flick snap, no fake open/close button and scrollable body.
- `pinLayout.ts` complete-link screen-space grouping at **all** zoom
  levels, including z20. Selected/alarming marker visible with +N and
  accessible in-place picker; GPS fixes are never rewritten.
- Undo **and redo** in Claude's on-map zone drawer.
- Existing `orun.fences.v1` pasture drafts read into the new
  `orun.app.zones.v1` zone book only when no new record exists. The legacy
  key is retained unchanged, and new writes are not acknowledged before
  successful persistent storage. Corrupt input stops modifications.
- PR #180's legacy `satelliteSafe=true` preference migrates to the
  corresponding Claude satellite zoom cap `maxZoom=15`. The new slider
  remains authoritative thereafter. No duplicate incompatible Esri
  overlay is added.

## Owner decision: TRACKER owns the geofence alarm
- No `OUTSIDE` status or AppEvent is synthesized from ordinary POSITION
  history on the phone. `buildHerd` shows **only informational inside-zone
  membership**, and `deriveEvents` does not call `outsideEpisodes`.
- Neither the phone nor backend writes/acknowledges a new fence on the
  collar here. The firmware's own confirmed OUTSIDE EVENT is the
  authoritative event; displaying such signed records is future work
  after event transport is implemented. Do not infer an active, delivered
  alarm from a local polygon.
- The previous phone-only `Serbest otlatma` alert-mute switch is **hidden**,
  because it never configured the collar. Grazing persistence remains
  readable for future schema compatibility but is not an active safety
  control. The app cannot promise that it muted real firmware alarms.

## Hardening
- Reject GeoJSON polygons with holes instead of filling excluded land.
- Validate *entire* new area set (all valid polygons, active 8/64 caps,
  unique ids) before accepting edits/imports.
- A failed localStorage write is an error, not a "saved" confirmation;
  surface this in the zone editor and detail/list view.
- Corrupt new-format or legacy-format zones are not overwritten.

## Regression checklist — MUST pass before another APK
```bash
cd edge/app
npm test
npm run build
```

Physical Android upgrade checks:
1. Keep current APK installed, update with debug APK of THIS commit.
2. Existing zones appear after upgrade (or a visible, non-destructive
   warning appears if legacy data cannot be read).
3. Tiny/fast swipe: peek > half > full > half > peek, scroll body normally.
4. Two collars at identical GPS coordinates on z20: +N selector works;
   their stored lat/lon remain unchanged.
5. Draw/edit zone; undo and redo; imported polygon with hole must be
   rejected, as must >8 active areas or >64 effective vertices.
6. Move an animal outside a locally drafted polygon: no phone-generated
   OUTSIDE red status, warning or alert. Genuine secure firmware EVENT
   reception is separately gated by transport integration.
7. Esri current/Wayback tiles over rural areas; if z17 returns opaque
   provider placeholder tiles, set tile cap to z15; z20 camera still works.
8. Test station BLE/USB handoff and server upload remain unchanged.

**Out of scope:** #182 offline OPFS/PMTiles M1 (still a draft foundation;
no downloadable regional map or offline basemap), authenticated collar
fence writer, actual OUTSIDE event transport.
