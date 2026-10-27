// MapLibre GL engine for the field map. Owns the map, the base styles and
// every overlay; React only hands it data. Overlays are re-added after each
// style change, so switching satellite / terrain / street keeps them.
//
// Bases (no account or key needed):
//   satellite — Esri World Imagery or one Esri Wayback release (+ place
//               names), asked up to the owner's tile limit (default 17) and
//               enlarged beyond: Esri answers missing 18-19 imagery with a
//               "Map data not yet available" picture, never an error
//   terrain   — OpenTopoMap, zoom 17 (overzoomed beyond)
//   street    — OpenFreeMap "liberty" vector style
//   3D relief — AWS Terrain Tiles (Terrarium DEM), zoom 15
// Without internet the base is blank but animals, trails and layers still draw.
//
// Smoothness: raster tiles switch without a fade; pins have no CSS filters
// (a hairline edge instead of a drop shadow) and scale with the zoom through
// one CSS variable set once per frame; data is re-sent only when it changed.
import {
  AttributionControl, GeoJSONSource, LngLatBounds, Map as MlMap, Marker, Popup, ScaleControl, setWorkerUrl,
  type MapMouseEvent, type StyleSpecification,
} from "maplibre-gl";
import "maplibre-gl/dist/maplibre-gl.css";
import type { Feature, FeatureCollection } from "geojson";
import workerUrl from "maplibre-gl/dist/maplibre-gl-worker.mjs?url";
import type { Cell } from "../../app/coverage.ts";
import type { LatLon } from "../../app/geo.ts";
import { agingLimit, epochMs, periodMs } from "../../app/herd.ts";
import { fmt } from "../../app/i18n/tr.ts";
import type { Animal, Observation } from "../../app/model.ts";
import type { BaseMap, Layers, SatPrefs, ZoneFill } from "../../app/stores.ts";
import { centroid, edgeM, type LonLat } from "../../app/zones.ts";
import { toneOfStatus } from "../../ds/components.tsx";
import { groupScreenPins } from "./pinLayout.ts";

setWorkerUrl(workerUrl);

const ESRI = "https://server.arcgisonline.com/ArcGIS/rest/services";
const WAYBACK = "https://wayback.maptiles.arcgis.com/arcgis/rest/services/World_Imagery/WMTS/1.0.0/GoogleMapsCompatible/MapServer/tile";
const ATTR = {
  esri: "Görüntü © Esri, Maxar, Earthstar Geographics",
  wayback: "Görüntü © Esri Wayback, Maxar, Earthstar Geographics",
  topo: "© OpenTopoMap (CC-BY-SA) · © OpenStreetMap katkıcıları",
  ofm: "OpenFreeMap © OpenMapTiles · © OpenStreetMap katkıcıları",
  dem: "Yükselti: AWS Terrain Tiles (Mapzen)",
};
const STREET_STYLE = "https://tiles.openfreemap.org/styles/liberty";
const DEM = "https://s3.amazonaws.com/elevation-tiles-prod/terrarium/{z}/{x}/{y}.png";

export interface MyPos extends LatLon { accuracy: number; heading: number | null; }
export interface StationPin { id: string; pos: LatLon; connected: boolean; }
export interface ZoneView { id: string; name: string; ring: LonLat[]; active: boolean; valid: boolean; }
export interface LinkLine { from: LatLon; to: LatLon; rssi: number; }

export interface MapData {
  herd: Animal[];
  selected: string | null;
  trail: { points: Observation[]; color: string } | null;     // selected animal, in range
  cursor: number | null;                                       // playback time (ms)
  follow: boolean;                                             // keep the playback marker in view
  heat: Observation[];
  coverage: Cell[];
  stations: StationPin[];
  me: MyPos | null;
  zones: ZoneView[];
  selectedZone: string | null;
  herdTrails: { id: string; color: string; points: Observation[] }[];  // every animal, in range
  links: LinkLine[];                                           // animal -> station, by signal
}

/** A boundary being drawn. `cross`: vertices are added at the screen centre. */
export interface Draft { ring: LonLat[]; cross: boolean; }

export interface EngineOpts {
  onSelect(id: string | null): void;
  onOffline(offline: boolean): void;
  onZoom?(zoom: number): void;
  onView?(bearing: number, pitch: number): void;
  /** Base tiles keep failing (true) or load again (false). */
  onTiles?(trouble: boolean): void;
  /** Drawing: a vertex was dragged or inserted. */
  onDraft?(ring: LonLat[]): void;
  /** Drawing in tap mode: the map was tapped there. */
  onDraftTap?(p: LonLat): void;
  /** Drawing in cross mode: the screen centre moved (live measurement). */
  onCentre?(p: LonLat): void;
}

const esc = (s: string) => s.replace(/[&<>"]/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;" })[c]!);
const RING = 2 * Math.PI * 23;
const LABEL_ZOOM = 15.5;
const TONE_VAR: Record<string, string> = { fresh: "var(--fresh)", aging: "var(--aging)", silent: "var(--silent)", alarm: "var(--alarm)" };
const COW = '<svg viewBox="0 0 256 256" width="20" height="20" fill="currentColor" aria-hidden="true"><path d="M120,128a16,16,0,1,1-16-16A16,16,0,0,1,120,128Zm32-16a16,16,0,1,0,16,16A16,16,0,0,0,152,112Zm84,20a20,20,0,0,1-20,20H204v15a38,38,0,0,1-22,69H74a38,38,0,0,1-22-69V152H40a20,20,0,0,1-20-20,60,60,0,0,1,36-55V48a12,12,0,0,1,24,0V70h96V48a12,12,0,0,1,24,0V77A60,60,0,0,1,236,132ZM180,198a14,14,0,0,0-14-14H90a14,14,0,0,0,0,28h76A14,14,0,0,0,180,198Z"/></svg>';

/**
 * Pin size by zoom (the rule carried over from the field prototype): a small
 * dot far out, half size in between, the full pin close in. 1 = full size.
 */
export function pinScale(zoom: number): number {
  return Math.max(0.42, Math.min(1, 0.42 + ((zoom - 12) / 4) * 0.58));
}

function ringSvg(color: string, ratio: number): string {
  const dash = Math.max(6, ratio * RING);
  // A hairline dark edge instead of a CSS drop shadow: same contrast on the
  // satellite image, nothing to repaint while the map moves.
  return `<svg viewBox="0 0 52 52" aria-hidden="true"><circle cx="26" cy="26" r="25" fill="#fff" stroke="rgba(0,0,0,.32)" stroke-width="1.2"/>` +
    `<circle cx="26" cy="26" r="23" fill="none" stroke="rgba(0,0,0,.08)" stroke-width="3.5"/>` +
    `<circle cx="26" cy="26" r="23" fill="none" stroke="${color}" stroke-width="var(--ring-w)" stroke-linecap="round" ` +
    `stroke-dasharray="${dash.toFixed(1)} ${RING.toFixed(1)}" transform="rotate(-90 26 26)"/></svg>`;
}

function pinHtml(a: Animal, selected: boolean, label: boolean): string {
  const tone = toneOfStatus(a.status);
  const dim = a.freshness === "stale" || a.freshness === "none";
  return `<span class="vis">${ringSvg(TONE_VAR[tone] ?? TONE_VAR.fresh, a.freshRatio)}` +
    `<span class="fc${dim ? " dim" : ""}" style="background:${a.color}">${a.initials ? esc(a.initials) : COW}</span>` +
    (tone === "alarm" ? `<span class="pulse"></span>` : "") + `</span>` +
    (label || selected ? `<span class="lbl">${esc(a.name)}</span>` : "");
}

function circlePolygon(c: LatLon, radiusM: number): [number, number][] {
  const out: [number, number][] = [];
  const kx = 111_320 * Math.cos((c.lat * Math.PI) / 180), ky = 110_540;
  for (let i = 0; i <= 48; i++) {
    const a = (i / 48) * 2 * Math.PI;
    out.push([c.lon + (radiusM * Math.cos(a)) / kx, c.lat + (radiusM * Math.sin(a)) / ky]);
  }
  return out;
}

const fc = (features: Feature[]): FeatureCollection => ({ type: "FeatureCollection", features });

function trailFeatures(points: Observation[], color: string, cursor: number | null): Feature[] {
  const out: Feature[] = [];
  for (let i = 1; i < points.length; i++) {
    const a = points[i - 1], b = points[i];
    const ta = epochMs(a.time), tb = epochMs(b.time);
    out.push({
      type: "Feature",
      properties: { color, gap: tb - ta > agingLimit(periodMs(a.period_s)), past: cursor === null || tb <= cursor },
      geometry: { type: "LineString", coordinates: [[a.lon, a.lat], [b.lon, b.lat]] },
    });
  }
  return out;
}

/** Continuous pieces of one animal's trail; long gaps are left out, not bridged. */
function herdTrailFeatures(list: MapData["herdTrails"]): Feature[] {
  const out: Feature[] = [];
  for (const t of list) {
    let run: [number, number][] = [];
    const flush = () => { if (run.length > 1) out.push({ type: "Feature", properties: { color: t.color }, geometry: { type: "LineString", coordinates: run } }); run = []; };
    for (let i = 0; i < t.points.length; i++) {
      const p = t.points[i];
      if (i > 0) {
        const q = t.points[i - 1];
        if (epochMs(p.time) - epochMs(q.time) > agingLimit(periodMs(q.period_s))) flush();
      }
      run.push([p.lon, p.lat]);
    }
    flush();
  }
  return out;
}

function rasterStyle(base: BaseMap, labels: boolean, sat: SatPrefs): StyleSpecification {
  const sources: StyleSpecification["sources"] = {};
  const layers: StyleSpecification["layers"] = [{ id: "bg", type: "background", paint: { "background-color": "#DDE3D6" } }];
  const crisp = { "raster-fade-duration": 0 };
  const maxzoom = Math.max(14, Math.min(19, Math.round(sat.maxZoom)));
  if (base === "satellite") {
    const tiles = sat.kind === "wayback" ? [`${WAYBACK}/${encodeURIComponent(sat.release)}/{z}/{y}/{x}`] : [`${ESRI}/World_Imagery/MapServer/tile/{z}/{y}/{x}`];
    sources.base = { type: "raster", tiles, tileSize: 256, maxzoom, attribution: sat.kind === "wayback" ? ATTR.wayback : ATTR.esri };
    layers.push({ id: "base", type: "raster", source: "base", paint: crisp });
    if (labels) {
      sources.labels = { type: "raster", tiles: [`${ESRI}/Reference/World_Boundaries_and_Places/MapServer/tile/{z}/{y}/{x}`], tileSize: 256, maxzoom };
      layers.push({ id: "labels", type: "raster", source: "labels", paint: { ...crisp, "raster-opacity": 0.9 } });
    }
  } else {
    sources.base = { type: "raster", tiles: ["a", "b", "c"].map((s) => `https://${s}.tile.opentopomap.org/{z}/{x}/{y}.png`), tileSize: 256, maxzoom: 17, attribution: ATTR.topo };
    layers.push({ id: "base", type: "raster", source: "base", paint: crisp });
  }
  return { version: 8, sources, layers };
}

const FALLBACK: StyleSpecification = { version: 8, sources: {}, layers: [{ id: "bg", type: "background", paint: { "background-color": "#DDE3D6" } }] };

/** Diagonal stripes for the "çizgili" zone fill. */
function hatchImage(): { width: number; height: number; data: Uint8Array } {
  const n = 12, data = new Uint8Array(n * n * 4);
  for (let y = 0; y < n; y++)
    for (let x = 0; x < n; x++) {
      const on = (x + y) % n < 2;
      const i = (y * n + x) * 4;
      data[i] = 255; data[i + 1] = 214; data[i + 2] = 10; data[i + 3] = on ? 150 : 0;
    }
  return { width: n, height: n, data };
}

export class MapEngine {
  readonly map: MlMap;
  private pins = new Map<string, Marker>();
  private clusters: Marker[] = [];
  private groupPopup: Popup | null = null;
  private stationPins = new Map<string, Marker>();
  private zoneLabels = new Map<string, Marker>();
  private edgeLabels: Marker[] = [];
  private vertexPins: Marker[] = [];
  private midPins: Marker[] = [];
  private meMarker: Marker | null = null;
  private ghost: Marker | null = null;
  private data: MapData | null = null;
  private layers: Layers | null = null;
  private base: BaseMap = "satellite";
  private sat: SatPrefs = { kind: "esri", release: "26334", maxZoom: 17 };
  private zoneFill: ZoneFill = "light";
  private tilt = false;
  private styleKey = "";
  private streetCache: StyleSpecification | null = null;
  private labelled = false;
  private resizer: ResizeObserver;
  private draft: Draft | null = null;
  private frame = 0;
  private tileErrors: number[] = [];
  private tileTrouble = false;
  private pinKey = "";

  private opts: EngineOpts;

  constructor(el: HTMLElement, opts: EngineOpts, view?: { center: [number, number]; zoom: number }) {
    this.opts = opts;
    this.map = new MlMap({
      container: el, style: FALLBACK, center: view?.center ?? [28.0546, 37.6107], zoom: view?.zoom ?? 14,
      maxZoom: 20, maxPitch: 75, attributionControl: false, fadeDuration: 150, dragRotate: true, touchPitch: true,
      refreshExpiredTiles: false,
    });
    this.map.addControl(new AttributionControl({ compact: true, customAttribution: ATTR.dem }), "bottom-right");
    this.map.addControl(new ScaleControl({ unit: "metric", maxWidth: 90 }), "bottom-left");
    // Attribution stays one tap away (the (i) button), not open over the map.
    const fold = () => el.querySelector(".maplibregl-ctrl-attrib")?.classList.remove("maplibregl-compact-show");
    this.map.on("load", fold);
    this.map.on("styledata", fold);
    this.map.on("rotate", () => this.opts.onView?.(this.map.getBearing(), this.map.getPitch()));
    this.map.on("pitch", () => this.opts.onView?.(this.map.getBearing(), this.map.getPitch()));
    this.resizer = new ResizeObserver(() => this.map.resize());
    this.resizer.observe(el);
    this.map.on("style.load", () => this.addOverlays());
    this.map.on("moveend", () => { this.recluster(); this.syncZoneLabels(); });
    this.map.on("zoomend", () => this.opts.onZoom?.(this.map.getZoom()));
    // Pin size follows the zoom continuously, once per frame.
    this.map.on("move", () => {
      if (this.frame) return;
      this.frame = requestAnimationFrame(() => { this.frame = 0; this.applyScale(); this.drawGuide(); });
    });
    this.applyScale();
    this.map.on("click", (e: MapMouseEvent) => {
      const t = e.originalEvent.target as HTMLElement;
      if (this.draft) {
        if (!this.draft.cross && !t.closest(".vtx, .mid")) this.opts.onDraftTap?.([e.lngLat.lng, e.lngLat.lat]);
        return;
      }
      if (!t.closest(".pin, .cluster")) this.opts.onSelect(null);
    });
    // Base tiles: a few failures in a short while means no imagery here now.
    this.map.on("error", (ev) => {
      const id = (ev as unknown as { sourceId?: string }).sourceId;
      if (id !== "base" && id !== "labels") return;
      const now = Date.now();
      this.tileErrors = [...this.tileErrors.filter((t) => now - t < 10_000), now];
      if (this.tileErrors.length >= 4 && !this.tileTrouble) { this.tileTrouble = true; this.opts.onTiles?.(true); }
    });
    this.map.on("sourcedata", (ev) => {
      const e = ev as unknown as { sourceId?: string; tile?: unknown };
      if (e.sourceId !== "base" || !e.tile || !this.tileTrouble) return;
      if (this.tileErrors.every((t) => Date.now() - t > 10_000)) { this.tileTrouble = false; this.opts.onTiles?.(false); }
    });
  }

  destroy(): void { if (this.frame) cancelAnimationFrame(this.frame); this.groupPopup?.remove(); this.resizer.disconnect(); this.map.remove(); }

  private applyScale(): void {
    const z = this.map.getZoom(), s = pinScale(z);
    const el = this.map.getContainer();
    el.style.setProperty("--ps", s.toFixed(3));
    const size = s < 0.62 ? "dot" : s < 0.9 ? "mid" : "full";
    if (el.dataset.pz !== size) el.dataset.pz = size;
    if ((z >= LABEL_ZOOM) !== this.labelled && this.data) { this.labelled = z >= LABEL_ZOOM; this.syncPins(this.data, true); }
  }

  // ------------------------------------------------------------- styling --
  async setBase(base: BaseMap, layers: Layers, sat?: SatPrefs): Promise<void> {
    if (sat) this.sat = sat;
    const s = this.sat;
    const key = `${base}|${base === "satellite" && layers.labels}|${base === "satellite" ? `${s.kind}:${s.release}:${s.maxZoom}` : ""}`;
    this.base = base;
    this.layers = layers;
    if (key === this.styleKey) { this.applyVisibility(); return; }
    this.styleKey = key;
    this.tileErrors = [];
    if (this.tileTrouble) { this.tileTrouble = false; this.opts.onTiles?.(false); }
    if (base !== "street") { this.map.setStyle(rasterStyle(base, layers.labels, s), { diff: false }); this.opts.onOffline(false); return; }
    try {
      if (!this.streetCache) {
        const ctl = new AbortController();
        const timer = setTimeout(() => ctl.abort(), 7000);
        const r = await fetch(STREET_STYLE, { signal: ctl.signal });
        clearTimeout(timer);
        if (!r.ok) throw new Error(String(r.status));
        this.streetCache = await r.json() as StyleSpecification;
      }
      if (this.styleKey !== key) return;
      this.map.setStyle(this.streetCache, { diff: false });
      this.opts.onOffline(false);
    } catch {
      if (this.styleKey !== key) return;
      this.map.setStyle(FALLBACK, { diff: false });
      this.opts.onOffline(true);
    }
  }

  setTilt(on: boolean): void {
    this.tilt = on;
    this.applyTerrain();
    this.map.easeTo({ pitch: on ? 62 : 0, duration: 700 });
  }

  resetNorth(): void { this.map.easeTo({ bearing: 0, pitch: this.tilt ? 62 : 0, duration: 500 }); }

  private applyTerrain(): void {
    if (!this.map.getSource("dem")) return;
    this.map.setTerrain(this.tilt && !this.draft ? { source: "dem", exaggeration: 1.35 } : null);
  }

  setZoneFill(f: ZoneFill): void {
    this.zoneFill = f;
    this.paintZoneFill();
  }

  private paintZoneFill(): void {
    const m = this.map;
    if (!m.getLayer("o-zone-fill")) return;
    m.setPaintProperty("o-zone-fill", "fill-opacity", this.zoneFill === "light" ? 0.14 : this.zoneFill === "hatch" ? 0.06 : 0);
    m.setLayoutProperty("o-zone-hatch", "visibility", this.zoneFill === "hatch" && this.layers?.zones !== false && !this.draft ? "visible" : "none");
  }

  private addOverlays(): void {
    const m = this.map;
    if (!m.getSource("dem")) m.addSource("dem", { type: "raster-dem", tiles: [DEM], tileSize: 256, maxzoom: 15, encoding: "terrarium" });
    if (!m.getSource("dem-shade")) m.addSource("dem-shade", { type: "raster-dem", tiles: [DEM], tileSize: 256, maxzoom: 15, encoding: "terrarium" });
    if (!m.hasImage("hatch")) m.addImage("hatch", hatchImage());
    const empty = fc([]);
    for (const id of ["o-coverage", "o-heat", "o-zones", "o-herd", "o-links", "o-trail", "o-trail-pts", "o-me", "o-draft", "o-guide"])
      if (!m.getSource(id)) m.addSource(id, { type: "geojson", data: empty });
    const add = (spec: Parameters<MlMap["addLayer"]>[0]) => { if (!m.getLayer(spec.id)) m.addLayer(spec); };
    add({ id: "o-relief", type: "hillshade", source: "dem-shade", layout: { visibility: "none" },
      paint: { "hillshade-exaggeration": 0.45, "hillshade-shadow-color": "#3A4A3F", "hillshade-highlight-color": "#FFFFFF" } });
    add({ id: "o-coverage", type: "fill", source: "o-coverage", minzoom: 14,
      paint: { "fill-color": ["match", ["get", "cls"], "strong", "#2E9E5B", "medium", "#E2A21C", "weak", "#D9472F", "#6B7280"], "fill-opacity": 0.55 } });
    add({ id: "o-heat", type: "heatmap", source: "o-heat",
      paint: {
        "heatmap-weight": ["get", "w"],
        "heatmap-radius": ["interpolate", ["linear"], ["zoom"], 12, 8, 16, 22, 19, 50],
        "heatmap-intensity": ["interpolate", ["linear"], ["zoom"], 12, 1, 18, 2.5],
        "heatmap-opacity": 0.75,
        "heatmap-color": ["interpolate", ["linear"], ["heatmap-density"], 0, "rgba(0,0,0,0)", 0.15, "rgba(200,241,105,0.45)",
          0.4, "rgba(226,162,28,0.7)", 0.7, "rgba(217,71,47,0.8)", 1, "rgba(150,20,20,0.9)"],
      } });
    // Zones: optional fill under everything; a dark casing and a yellow edge
    // read on satellite and topo alike. Switched-off zones are grey dashed,
    // zones the collar would refuse are red dashed, the selected one white.
    add({ id: "o-zone-fill", type: "fill", source: "o-zones", filter: ["==", ["get", "active"], true],
      paint: { "fill-color": "#FFD60A", "fill-opacity": 0.14 } });
    add({ id: "o-zone-hatch", type: "fill", source: "o-zones", filter: ["==", ["get", "active"], true], layout: { visibility: "none" },
      paint: { "fill-pattern": "hatch" } });
    add({ id: "o-zone-casing", type: "line", source: "o-zones", layout: { "line-join": "round" },
      paint: { "line-color": "rgba(0,0,0,0.55)", "line-width": ["case", ["get", "sel"], 7, 5] } });
    add({ id: "o-zone-line", type: "line", source: "o-zones", filter: ["all", ["==", ["get", "active"], true], ["==", ["get", "valid"], true]],
      layout: { "line-join": "round" },
      paint: { "line-color": ["case", ["get", "sel"], "#FFFFFF", "#FFD60A"], "line-width": ["case", ["get", "sel"], 3.2, 2.4] } });
    add({ id: "o-zone-off", type: "line", source: "o-zones", filter: ["any", ["==", ["get", "active"], false], ["==", ["get", "valid"], false]],
      layout: { "line-join": "round" },
      paint: { "line-color": ["case", ["get", "sel"], "#FFFFFF", ["==", ["get", "valid"], false], "#FF7A66", "#C9D3DA"],
        "line-width": 2.2, "line-dasharray": [3, 2] } });
    add({ id: "o-herd-trail", type: "line", source: "o-herd", layout: { "line-cap": "round", "line-join": "round" },
      paint: { "line-color": ["get", "color"], "line-width": 2, "line-opacity": 0.6 } });
    add({ id: "o-links", type: "line", source: "o-links",
      paint: { "line-color": ["get", "color"], "line-width": 1.6, "line-opacity": 0.75, "line-dasharray": [2, 2] } });
    add({ id: "o-trail-casing", type: "line", source: "o-trail", filter: ["==", ["get", "gap"], false], layout: { "line-cap": "round", "line-join": "round" },
      paint: { "line-color": "#FFFFFF", "line-width": 6, "line-opacity": ["case", ["get", "past"], 0.85, 0.3] } });
    add({ id: "o-trail", type: "line", source: "o-trail", filter: ["==", ["get", "gap"], false], layout: { "line-cap": "round", "line-join": "round" },
      paint: { "line-color": ["get", "color"], "line-width": 3.5, "line-opacity": ["case", ["get", "past"], 1, 0.3] } });
    add({ id: "o-trail-gap", type: "line", source: "o-trail", filter: ["==", ["get", "gap"], true],
      paint: { "line-color": "#FFFFFF", "line-width": 2.5, "line-dasharray": [1.5, 2], "line-opacity": ["case", ["get", "past"], 0.9, 0.3] } });
    // Trail points darken from old to new, so direction and time read at a glance.
    add({ id: "o-trail-pts", type: "circle", source: "o-trail-pts", minzoom: 14,
      paint: { "circle-radius": 3, "circle-color": ["get", "color"], "circle-stroke-color": "#FFFFFF", "circle-stroke-width": 1.5,
        "circle-opacity": ["case", ["get", "past"], ["interpolate", ["linear"], ["get", "k"], 0, 0.25, 1, 1], 0.3],
        "circle-stroke-opacity": ["case", ["get", "past"], ["interpolate", ["linear"], ["get", "k"], 0, 0.35, 1, 1], 0.3] } });
    add({ id: "o-me", type: "fill", source: "o-me", paint: { "fill-color": "#2F6FD0", "fill-opacity": 0.14, "fill-outline-color": "#2F6FD0" } });
    add({ id: "o-draft-fill", type: "fill", source: "o-draft", filter: ["==", ["geometry-type"], "Polygon"],
      paint: { "fill-color": "#FFD60A", "fill-opacity": 0.18 } });
    add({ id: "o-draft-casing", type: "line", source: "o-draft", layout: { "line-cap": "round", "line-join": "round" },
      paint: { "line-color": "rgba(0,0,0,0.5)", "line-width": 6 } });
    add({ id: "o-draft-line", type: "line", source: "o-draft", layout: { "line-cap": "round", "line-join": "round" },
      paint: { "line-color": "#FFFFFF", "line-width": 3 } });
    add({ id: "o-guide", type: "line", source: "o-guide",
      paint: { "line-color": "#FFFFFF", "line-width": ["case", ["get", "close"], 2, 2.5], "line-opacity": ["case", ["get", "close"], 0.55, 1], "line-dasharray": [2, 2] } });
    try {
      m.setSky({ "sky-color": "#9CC7EE", "horizon-color": "#E6EEF2", "fog-color": "#E6EEF2",
        "sky-horizon-blend": 0.6, "horizon-fog-blend": 0.5, "fog-ground-blend": 0.4 });
    } catch { /* the sky is decoration */ }
    this.applyTerrain();
    this.applyVisibility();
    this.paintZoneFill();
    this.sent = {};
    if (this.data) this.pushData(this.data, true);
    this.pushDraft();
  }

  private applyVisibility(): void {
    const l = this.layers, m = this.map;
    if (!l || !m.getLayer("o-heat")) return;
    const drawing = this.draft !== null;
    const vis = (id: string, on: boolean) => { if (m.getLayer(id)) m.setLayoutProperty(id, "visibility", on ? "visible" : "none"); };
    vis("o-relief", l.relief && this.base !== "street");
    vis("o-coverage", l.coverage && !drawing);
    vis("o-heat", l.heat && !drawing);
    for (const id of ["o-trail-casing", "o-trail", "o-trail-gap", "o-trail-pts"]) vis(id, l.trail && !drawing);
    vis("o-herd-trail", l.herdTrails && !drawing);
    vis("o-links", l.links && !drawing);
    for (const id of ["o-zone-fill", "o-zone-casing", "o-zone-line", "o-zone-off"]) vis(id, l.zones || drawing);
    this.paintZoneFill();
    vis("o-me", l.me);
    const animals = l.animals && !drawing ? "" : "none";
    for (const mk of this.pins.values()) mk.getElement().style.display = animals;
    for (const mk of this.clusters) mk.getElement().style.display = animals;
    for (const mk of this.stationPins.values()) mk.getElement().style.display = l.stations && !drawing ? "" : "none";
    for (const mk of this.zoneLabels.values()) mk.getElement().style.display = l.zones && !drawing ? "" : "none";
    if (this.meMarker) this.meMarker.getElement().style.display = l.me ? "" : "none";
  }

  setLayers(layers: Layers): void {
    const relabel = this.base === "satellite" && this.layers?.labels !== layers.labels;
    this.layers = layers;
    if (relabel) { void this.setBase(this.base, layers); return; }
    this.applyVisibility();
  }

  // ---------------------------------------------------------------- data --
  setData(d: MapData): void {
    this.data = d;
    if (this.map.getSource("o-heat")) this.pushData(d);
    this.syncPins(d);
    this.syncStations(d.stations);
    this.syncMe(d.me);
    this.syncGhost(d);
    this.syncZoneLabels();
    // Clusters only move when a pin moved, appeared, changed colour or was selected.
    const key = d.herd.map((a) => (a.last ? `${a.id}:${a.last.lat}:${a.last.lon}:${toneOfStatus(a.status)}` : "")).join("|") + `#${d.selected}`;
    if (key !== this.pinKey) { this.pinKey = key; this.recluster(); }
  }

  // Only sources whose input changed are re-sent (playback ticks often, the
  // clock ticks every 30 s): keys are content, not object identity.
  private sent: Partial<Record<string, unknown>> = {};
  private pushData(d: MapData, force = false): void {
    const set = (id: string, key: unknown, make: () => FeatureCollection) => {
      if (!force && this.sent[id] === key) return;
      const src = this.map.getSource(id) as GeoJSONSource | undefined;
      if (!src) return;
      this.sent[id] = key;
      void src.setData(make());
    };
    const listKey = (pts: Observation[]) => `${pts.length}:${pts[0]?.time}:${pts[pts.length - 1]?.time}`;
    set("o-coverage", `${d.coverage.length}:${d.coverage[0]?.ring[0]}:${d.coverage[d.coverage.length - 1]?.ring[0]}:${d.coverage.map((c) => c.cls[0]).join("")}`,
      () => fc(d.coverage.map((c) => ({ type: "Feature", properties: { cls: c.cls }, geometry: { type: "Polygon", coordinates: [c.ring] } }))));
    // Weight so the total stays comparable whether 50 or 50 000 records are shown.
    const w = Math.max(0.015, Math.min(1, 120 / Math.max(1, d.heat.length)));
    set("o-heat", listKey(d.heat), () => fc(d.heat.map((p) => ({ type: "Feature", properties: { w }, geometry: { type: "Point", coordinates: [p.lon, p.lat] } }))));
    const trailKey = d.trail ? `${d.selected}:${listKey(d.trail.points)}:${d.trail.color}:${d.cursor === null ? "" : Math.floor(d.cursor / 60_000)}` : "none";
    set("o-trail", trailKey, () => fc(d.trail ? trailFeatures(d.trail.points, d.trail.color, d.cursor) : []));
    set("o-trail-pts", trailKey, () => {
      if (!d.trail) return fc([]);
      const n = Math.max(1, d.trail.points.length - 1);
      return fc(d.trail.points.map((p, i) => ({
        type: "Feature", properties: { color: d.trail!.color, k: i / n, past: d.cursor === null || epochMs(p.time) <= d.cursor },
        geometry: { type: "Point", coordinates: [p.lon, p.lat] },
      })));
    });
    set("o-herd", d.herdTrails.map((t) => `${t.id}:${t.color}:${listKey(t.points)}`).join("|"), () => fc(herdTrailFeatures(d.herdTrails)));
    set("o-links", d.links.map((l) => `${l.from.lat},${l.from.lon},${l.to.lat},${l.to.lon},${l.rssi}`).join("|"), () => fc(d.links.map((l) => ({
      type: "Feature", properties: { color: l.rssi > -105 ? "#3FCF7F" : l.rssi > -115 ? "#F2B632" : "#FF5C50" },
      geometry: { type: "LineString", coordinates: [[l.from.lon, l.from.lat], [l.to.lon, l.to.lat]] },
    }))));
    set("o-zones", JSON.stringify([d.zones.map((z) => [z.id, z.active, z.valid, z.ring.length, z.ring[0]]), d.selectedZone, d.zones.map((z) => z.ring)]), () => fc(d.zones.map((z) => ({
      type: "Feature", properties: { id: z.id, active: z.active, valid: z.valid, sel: z.id === d.selectedZone },
      geometry: { type: "Polygon", coordinates: [[...z.ring, z.ring[0]]] },
    }))));
    set("o-me", d.me ? `${d.me.lat},${d.me.lon},${Math.round(d.me.accuracy)}` : "none",
      () => fc(d.me ? [{ type: "Feature", properties: {}, geometry: { type: "Polygon", coordinates: [circlePolygon(d.me, Math.max(5, d.me.accuracy))] } }] : []));
  }

  private syncPins(d: MapData, force = false): void {
    const label = this.labelled;
    const keep = new Set<string>();
    const hidden = this.layers?.animals === false || this.draft !== null;
    for (const a of d.herd) {
      if (!a.last) continue;
      keep.add(a.id);
      let mk = this.pins.get(a.id);
      const html = pinHtml(a, a.id === d.selected, label);
      if (!mk) {
        const el = document.createElement("button");
        el.className = "pin";
        el.setAttribute("aria-label", a.name);
        el.addEventListener("click", (e) => { e.stopPropagation(); this.opts.onSelect(el.dataset.id ?? null); });
        mk = new Marker({ element: el, anchor: "center" }).setLngLat([a.last.lon, a.last.lat]).addTo(this.map);
        this.pins.set(a.id, mk);
      }
      const el = mk.getElement();
      el.dataset.id = a.id;
      if (force || el.dataset.html !== html) { el.innerHTML = html; el.dataset.html = html; }
      el.classList.toggle("selected", a.id === d.selected);
      el.classList.toggle("alarm", toneOfStatus(a.status) === "alarm");
      el.style.zIndex = a.id === d.selected ? "6" : String(Math.min(5, a.attention));
      el.style.display = hidden ? "none" : "";
      mk.setLngLat([a.last.lon, a.last.lat]);
    }
    for (const [id, mk] of this.pins) if (!keep.has(id)) { mk.remove(); this.pins.delete(id); }
  }

  /**
   * Overlapping pins merge into a count bubble until zoomed in. The overlap
   * distance follows the pin size, and an alarm or the selected animal never
   * hides inside a bubble.
   */
  private recluster(): void {
    const d = this.data;
    for (const c of this.clusters) c.remove();
    this.clusters = [];
    if (!d) return;
    const zoom = this.map.getZoom();
    for (const mk of this.pins.values()) {
      const el = mk.getElement();
      el.classList.remove("in-cluster");
      el.querySelector(".pin-count")?.remove();
    }

    // This must run even at z20: collars at an identical real GNSS fix
    // cannot be separated by zooming. Group only projected screen pixels;
    // never modify the underlying lat/lon values.
    const animals = d.herd.filter((a) => a.last);
    const byId = new Map(animals.map((a) => [a.id, a] as const));
    const groups = groupScreenPins(animals.map((a) => {
      const p = this.map.project([a.last!.lon, a.last!.lat]);
      return { id: a.id, x: p.x, y: p.y, selected: a.id === d.selected };
    }), zoom);

    for (const group of groups) {
      if (group.ids.length < 2) continue;
      const members = group.ids.map((id) => byId.get(id)!).filter(Boolean);
      const mean: [number, number] = [
        members.reduce((n, a) => n + a.last!.lon, 0) / members.length,
        members.reduce((n, a) => n + a.last!.lat, 0) / members.length,
      ];
      // Keep the selected pin or the most important alarm visibly in front.
      // The other collars remain individually selectable via the +N picker.
      const exposed = (group.selectedId && byId.get(group.selectedId)) ??
        members.find((a) => toneOfStatus(a.status) === "alarm") ?? null;
      if (exposed) {
        for (const a of members)
          if (a.id !== exposed.id) this.pins.get(a.id)?.getElement().classList.add("in-cluster");
        const el = this.pins.get(exposed.id)?.getElement();
        if (el) {
          // The parent pin already is a button. Do not nest another button.
          const badge = document.createElement("span");
          badge.className = "pin-count";
          badge.textContent = `+${members.length - 1}`;
          badge.setAttribute("aria-label", `${members.length - 1} hayvan daha: seçim listesini aç`);
          badge.addEventListener("click", (ev) => {
            ev.stopPropagation();
            this.showGroupChoices(members, [exposed.last!.lon, exposed.last!.lat]);
          });
          el.appendChild(badge);
        }
        continue;
      }

      for (const a of members) this.pins.get(a.id)?.getElement().classList.add("in-cluster");
      const worst = members.reduce((w, a) => a.attention > w.attention ? a : w, members[0]);
      const el = document.createElement("button");
      el.type = "button";
      el.className = `cluster c-${toneOfStatus(worst.status)}`;
      el.setAttribute("aria-label", `${members.length} hayvan aynı bölgede; seçmek için dokun`);
      const vis = document.createElement("span");
      vis.className = "vis";
      const number = document.createElement("b");
      number.textContent = String(members.length);
      vis.appendChild(number);
      el.appendChild(vis);
      el.style.display = this.layers?.animals === false ? "none" : "";
      el.addEventListener("click", (ev) => {
        ev.stopPropagation();
        const first = members[0].last!;
        const spread = members.some((a) => Math.abs(a.last!.lon - first.lon) > 1e-7 ||
          Math.abs(a.last!.lat - first.lat) > 1e-7);
        if (zoom < 18.5 && spread) {
          const bounds = new LngLatBounds();
          for (const a of members) bounds.extend([a.last!.lon, a.last!.lat]);
          this.map.fitBounds(bounds, { padding: 90, maxZoom: 19.5, duration: 600 });
        } else this.showGroupChoices(members, mean);
      });
      this.clusters.push(new Marker({ element: el, anchor: "center" }).setLngLat(mean).addTo(this.map));
    }
  }

  private showGroupChoices(members: Animal[], at: [number, number]): void {
    this.groupPopup?.remove();
    const box = document.createElement("div");
    box.className = "pin-choices";
    const title = document.createElement("strong");
    title.textContent = `${members.length} hayvan bu konumda`;
    box.appendChild(title);
    for (const a of members) {
      const button = document.createElement("button");
      button.type = "button";
      button.textContent = a.name;
      button.addEventListener("click", (ev) => {
        ev.stopPropagation();
        this.groupPopup?.remove();
        this.groupPopup = null;
        this.opts.onSelect(a.id);
      });
      box.appendChild(button);
    }
    this.groupPopup = new Popup({ offset: 20, maxWidth: "240px", closeButton: true, closeOnClick: true })
      .setDOMContent(box).setLngLat(at).addTo(this.map);
  }

  /** Zone names at their centre when the zone is wide enough on screen; edge lengths on the selected one. */
  private syncZoneLabels(): void {
    const d = this.data;
    const zones = d?.zones ?? [];
    const keep = new Set<string>();
    const show = this.layers?.zones !== false && !this.draft;
    for (const z of zones) {
      if (z.ring.length < 3) continue;
      const xs = z.ring.map((p) => this.map.project(p).x);
      const wide = Math.max(...xs) - Math.min(...xs) > 80;
      if (!wide && z.id !== d?.selectedZone) continue;
      keep.add(z.id);
      let mk = this.zoneLabels.get(z.id);
      const text = z.active ? z.name : `${z.name} · kapalı`;
      if (!mk) {
        const el = document.createElement("div");
        el.className = "zone-lbl";
        mk = new Marker({ element: el, anchor: "center" }).setLngLat(centroid(z.ring)).addTo(this.map);
        this.zoneLabels.set(z.id, mk);
      }
      const el = mk.getElement();
      if (el.textContent !== text) el.textContent = text;
      el.classList.toggle("off", !z.active);
      el.style.display = show ? "" : "none";
      mk.setLngLat(centroid(z.ring));
    }
    for (const [id, mk] of this.zoneLabels) if (!keep.has(id)) { mk.remove(); this.zoneLabels.delete(id); }

    for (const m of this.edgeLabels) m.remove();
    this.edgeLabels = [];
    const sel = zones.find((z) => z.id === d?.selectedZone);
    if (sel && show) this.edgeLabels = this.lengthLabels(sel.ring, true, 70);
  }

  private lengthLabels(ring: LonLat[], closed: boolean, minPx: number): Marker[] {
    const out: Marker[] = [];
    const n = ring.length, segs = closed && n >= 3 ? n : n - 1;
    for (let i = 0; i < segs; i++) {
      const a = ring[i], b = ring[(i + 1) % n];
      const pa = this.map.project(a), pb = this.map.project(b);
      if (Math.hypot(pb.x - pa.x, pb.y - pa.y) < minPx) continue;
      const el = document.createElement("div");
      el.className = "edge-lbl";
      el.textContent = fmt.distance(edgeM(a, b));
      out.push(new Marker({ element: el, anchor: "bottom", offset: [0, -6] }).setLngLat([(a[0] + b[0]) / 2, (a[1] + b[1]) / 2]).addTo(this.map));
    }
    return out;
  }

  // ------------------------------------------------------------- drawing --
  /** Start, update or end (null) boundary drawing. */
  setDraft(d: Draft | null): void {
    const was = this.draft !== null;
    this.draft = d;
    if (was !== (d !== null)) {
      if (d) this.map.easeTo({ pitch: 0, bearing: 0, duration: 300 });
      this.applyTerrain();
      this.applyVisibility();
      if (this.data) { this.syncPins(this.data); this.recluster(); }
      this.syncZoneLabels();
    }
    this.pushDraft();
  }

  /** The screen centre: where the next vertex goes in cross mode. */
  centre(): LonLat { const c = this.map.getCenter(); return [c.lng, c.lat]; }

  private pushDraft(): void {
    const src = this.map.getSource("o-draft") as GeoJSONSource | undefined;
    for (const m of [...this.vertexPins, ...this.midPins]) m.remove();
    this.vertexPins = []; this.midPins = [];
    for (const m of this.edgeLabels) m.remove();
    this.edgeLabels = [];
    const d = this.draft;
    if (!d || !d.cross) void (this.map.getSource("o-guide") as GeoJSONSource | undefined)?.setData(fc([]));
    if (!d) {
      void src?.setData(fc([]));
      this.syncZoneLabels();
      return;
    }
    const r = d.ring;
    const shape: Feature[] = [];
    if (r.length >= 3 && !d.cross) shape.push({ type: "Feature", properties: {}, geometry: { type: "Polygon", coordinates: [[...r, r[0]]] } });
    else if (r.length >= 2) shape.push({ type: "Feature", properties: {}, geometry: { type: "LineString", coordinates: r } });
    void src?.setData(fc(shape));

    // Vertices: drag to move. Midpoints: tap to add a corner there.
    r.forEach((p, i) => {
      const el = document.createElement("div");
      el.className = "vtx" + (i === 0 ? " first" : "");
      el.setAttribute("aria-label", `Köşe ${i + 1}`);
      const mk = new Marker({ element: el, anchor: "center", draggable: true }).setLngLat(p).addTo(this.map);
      mk.on("drag", () => {
        const ll = mk.getLngLat();
        const next = r.map((q, k) => (k === i ? [ll.lng, ll.lat] as LonLat : q));
        void src?.setData(fc(next.length >= 3 && !d.cross ? [{ type: "Feature", properties: {}, geometry: { type: "Polygon", coordinates: [[...next, next[0]]] } }]
          : [{ type: "Feature", properties: {}, geometry: { type: "LineString", coordinates: next } }]));
      });
      mk.on("dragend", () => { const ll = mk.getLngLat(); this.opts.onDraft?.(r.map((q, k) => (k === i ? [ll.lng, ll.lat] as LonLat : q))); });
      this.vertexPins.push(mk);
    });
    const closed = !d.cross && r.length >= 3;
    const segs = closed ? r.length : r.length - 1;
    for (let i = 0; i < segs; i++) {
      const a = r[i], b = r[(i + 1) % r.length];
      const pa = this.map.project(a), pb = this.map.project(b);
      if (Math.hypot(pb.x - pa.x, pb.y - pa.y) < 56) continue;
      const el = document.createElement("button");
      el.className = "mid";
      el.setAttribute("aria-label", "Buraya köşe ekle");
      el.addEventListener("click", (e) => {
        e.stopPropagation();
        const next = [...r];
        next.splice(i + 1, 0, [(a[0] + b[0]) / 2, (a[1] + b[1]) / 2]);
        this.opts.onDraft?.(next);
      });
      this.midPins.push(new Marker({ element: el, anchor: "center" }).setLngLat([(a[0] + b[0]) / 2, (a[1] + b[1]) / 2]).addTo(this.map));
    }
    this.edgeLabels = this.lengthLabels(r, closed, 64);
    this.drawGuide();
  }

  /** Cross mode: dashed line from the last vertex to the centre (and back to the first). */
  private drawGuide(): void {
    const d = this.draft;
    if (!d || !d.cross) return;
    const g = this.map.getSource("o-guide") as GeoJSONSource | undefined;
    const c = this.centre();
    this.opts.onCentre?.(c);
    if (!g) return;
    const r = d.ring;
    const f: Feature[] = [];
    if (r.length) f.push({ type: "Feature", properties: { close: false }, geometry: { type: "LineString", coordinates: [r[r.length - 1], c] } });
    if (r.length >= 2) f.push({ type: "Feature", properties: { close: true }, geometry: { type: "LineString", coordinates: [c, r[0]] } });
    void g.setData(fc(f));
  }

  // ------------------------------------------------------------- markers --
  private syncStations(list: StationPin[]): void {
    const keep = new Set(list.map((s) => s.id));
    for (const s of list) {
      let mk = this.stationPins.get(s.id);
      if (!mk) {
        const el = document.createElement("div");
        el.className = "station-pin";
        el.innerHTML = '<svg viewBox="0 0 256 256" width="20" height="20" fill="currentColor" aria-hidden="true"><path d="M128,88a40,40,0,1,0,40,40A40,40,0,0,0,128,88Zm0,64a24,24,0,1,1,24-24A24,24,0,0,1,128,152ZM73.11,182.89a8,8,0,0,1-11.32,0,96,96,0,0,1,0-135.78,8,8,0,0,1,11.32,11.32,80,80,0,0,0,0,113.14A8,8,0,0,1,73.11,182.89Zm121.1-135.78a96,96,0,0,1,0,135.78,8,8,0,0,1-11.32-11.32,80,80,0,0,0,0-113.14,8,8,0,0,1,11.32-11.32Z"/></svg>';
        mk = new Marker({ element: el, anchor: "center" }).setLngLat([s.pos.lon, s.pos.lat]).addTo(this.map);
        this.stationPins.set(s.id, mk);
      }
      mk.getElement().classList.toggle("on", s.connected);
      mk.getElement().style.display = this.layers?.stations === false || this.draft ? "none" : "";
      mk.setLngLat([s.pos.lon, s.pos.lat]);
    }
    for (const [id, mk] of this.stationPins) if (!keep.has(id)) { mk.remove(); this.stationPins.delete(id); }
  }

  private syncMe(me: MyPos | null): void {
    if (!me) { this.meMarker?.remove(); this.meMarker = null; return; }
    if (!this.meMarker) {
      const el = document.createElement("div");
      el.className = "me-pin";
      el.innerHTML = '<span class="cone"></span><span class="dot"></span>';
      this.meMarker = new Marker({ element: el, anchor: "center", rotationAlignment: "map" }).setLngLat([me.lon, me.lat]).addTo(this.map);
    }
    this.meMarker.setLngLat([me.lon, me.lat]);
    const cone = this.meMarker.getElement().querySelector(".cone") as HTMLElement;
    cone.style.display = me.heading === null ? "none" : "";
    if (me.heading !== null) this.meMarker.setRotation(me.heading);
    this.meMarker.getElement().style.display = this.layers?.me === false ? "none" : "";
  }

  private syncGhost(d: MapData): void {
    if (d.cursor === null || !d.trail || !d.trail.points.length) { this.ghost?.remove(); this.ghost = null; return; }
    let at = d.trail.points[0];
    for (const p of d.trail.points) { if (epochMs(p.time) <= d.cursor) at = p; else break; }
    if (!this.ghost) {
      const el = document.createElement("div");
      el.className = "ghost-pin";
      this.ghost = new Marker({ element: el, anchor: "center" }).setLngLat([at.lon, at.lat]).addTo(this.map);
    }
    this.ghost.getElement().style.background = d.trail.color;
    this.ghost.setLngLat([at.lon, at.lat]);
    if (d.follow) {
      const p = this.map.project([at.lon, at.lat]), c = this.map.getContainer();
      const pad = this.map.getPadding();
      const [l, r, t, b] = [pad.left ?? 0, pad.right ?? 0, pad.top ?? 0, pad.bottom ?? 0];
      if (p.x < l || p.x > c.clientWidth - r || p.y < t || p.y > c.clientHeight - b)
        this.map.easeTo({ center: [at.lon, at.lat], duration: 400 });
    }
  }

  // -------------------------------------------------------------- camera --
  /** `flat`: no padding at all, so the padded centre is the screen centre (the drawing cross). */
  setPadding(bottom: number, top = 140, flat = false): void {
    this.map.setPadding(flat ? { top: 0, bottom: 0, left: 0, right: 0 } : { top, bottom, left: 32, right: 88 });
    this.map.getContainer().style.setProperty("--sheet-h", `${bottom}px`);
  }

  fit(points: LatLon[], maxZoom = 17): void {
    if (!points.length) return;
    if (points.length === 1) { this.map.easeTo({ center: [points[0].lon, points[0].lat], zoom: Math.max(this.map.getZoom(), 16), duration: 600 }); return; }
    const b = new LngLatBounds();
    for (const p of points) b.extend([p.lon, p.lat]);
    this.map.fitBounds(b, { padding: 48, maxZoom, duration: 700 });
  }

  fitRing(ring: LonLat[], maxZoom = 18): void {
    if (!ring.length) return;
    const b = new LngLatBounds();
    for (const p of ring) b.extend(p);
    this.map.fitBounds(b, { padding: 56, maxZoom, duration: 600 });
  }

  flyTo(p: LatLon, zoom?: number): void {
    this.map.easeTo({ center: [p.lon, p.lat], zoom: zoom ?? Math.max(this.map.getZoom(), 16.5), duration: 600 });
  }

  view(): { center: [number, number]; zoom: number } {
    const c = this.map.getCenter();
    return { center: [c.lng, c.lat], zoom: this.map.getZoom() };
  }
}
