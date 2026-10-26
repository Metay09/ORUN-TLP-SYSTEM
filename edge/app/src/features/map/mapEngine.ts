// MapLibre GL engine for the field map. Owns the map, the base styles and
// every overlay; React only hands it data. Overlays are re-added after each
// style change, so switching satellite / terrain / street keeps them.
//
// Bases (no account or key needed):
//   satellite — Esri World Imagery (+ place names overlay), zoom 19
//   terrain   — OpenTopoMap, zoom 17 (overzoomed beyond)
//   street    — OpenFreeMap "liberty" vector style
//   3D relief — AWS Terrain Tiles (Terrarium DEM), zoom 15
// Without internet the base is blank but animals, trails and layers still draw.
import {
  AttributionControl, GeoJSONSource, LngLatBounds, Map as MlMap, Marker, setWorkerUrl,
  type StyleSpecification,
} from "maplibre-gl";
import "maplibre-gl/dist/maplibre-gl.css";
import type { Feature, FeatureCollection } from "geojson";
import workerUrl from "maplibre-gl/dist/maplibre-gl-worker.mjs?url";
import type { Cell } from "../../app/coverage.ts";
import type { LatLon } from "../../app/geo.ts";
import { agingLimit, epochMs, periodMs } from "../../app/herd.ts";
import type { Animal, Observation } from "../../app/model.ts";
import type { BaseMap, Layers } from "../../app/stores.ts";
import { toneOfStatus } from "../../ds/components.tsx";

setWorkerUrl(workerUrl);

const ESRI = "https://server.arcgisonline.com/ArcGIS/rest/services";
const ATTR = {
  esri: "Görüntü © Esri, Maxar, Earthstar Geographics",
  topo: "© OpenTopoMap (CC-BY-SA) · © OpenStreetMap katkıcıları",
  ofm: "OpenFreeMap © OpenMapTiles · © OpenStreetMap katkıcıları",
  dem: "Yükselti: AWS Terrain Tiles (Mapzen)",
};
const STREET_STYLE = "https://tiles.openfreemap.org/styles/liberty";
const DEM = "https://s3.amazonaws.com/elevation-tiles-prod/terrarium/{z}/{x}/{y}.png";

export interface MyPos extends LatLon { accuracy: number; heading: number | null; }
export interface StationPin { id: string; pos: LatLon; connected: boolean; }

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
}

export interface EngineOpts {
  onSelect(id: string | null): void;
  onOffline(offline: boolean): void;
  onZoom?(zoom: number): void;
  onView?(bearing: number, pitch: number): void;
}

const esc = (s: string) => s.replace(/[&<>"]/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;" })[c]!);
const RING = 2 * Math.PI * 23;
const LABEL_ZOOM = 16.5;
const TONE_VAR: Record<string, string> = { fresh: "var(--fresh)", aging: "var(--aging)", silent: "var(--silent)", alarm: "var(--alarm)" };
const COW = '<svg viewBox="0 0 256 256" width="20" height="20" fill="currentColor" aria-hidden="true"><path d="M120,128a16,16,0,1,1-16-16A16,16,0,0,1,120,128Zm32-16a16,16,0,1,0,16,16A16,16,0,0,0,152,112Zm84,20a20,20,0,0,1-20,20H204v15a38,38,0,0,1-22,69H74a38,38,0,0,1-22-69V152H40a20,20,0,0,1-20-20,60,60,0,0,1,36-55V48a12,12,0,0,1,24,0V70h96V48a12,12,0,0,1,24,0V77A60,60,0,0,1,236,132ZM180,198a14,14,0,0,0-14-14H90a14,14,0,0,0,0,28h76A14,14,0,0,0,180,198Z"/></svg>';

function ringSvg(color: string, ratio: number): string {
  const dash = Math.max(6, ratio * RING);
  return `<svg viewBox="0 0 52 52" aria-hidden="true"><circle cx="26" cy="26" r="25" fill="#fff"/>` +
    `<circle cx="26" cy="26" r="23" fill="none" stroke="rgba(0,0,0,.08)" stroke-width="3.5"/>` +
    `<circle cx="26" cy="26" r="23" fill="none" stroke="${color}" stroke-width="var(--ring-w)" stroke-linecap="round" ` +
    `stroke-dasharray="${dash.toFixed(1)} ${RING.toFixed(1)}" transform="rotate(-90 26 26)"/></svg>`;
}

function pinHtml(a: Animal, selected: boolean, label: boolean): string {
  const tone = toneOfStatus(a.status);
  const dim = a.freshness === "stale" || a.freshness === "none";
  return `${ringSvg(TONE_VAR[tone] ?? TONE_VAR.fresh, a.freshRatio)}` +
    `<span class="fc${dim ? " dim" : ""}" style="background:${a.color}">${a.initials ? esc(a.initials) : COW}</span>` +
    (label || selected ? `<span class="lbl">${esc(a.name)}</span>` : "") +
    (tone === "alarm" ? `<span class="pulse"></span>` : "");
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

function rasterStyle(base: BaseMap, labels: boolean): StyleSpecification {
  const sources: StyleSpecification["sources"] = {};
  const layers: StyleSpecification["layers"] = [{ id: "bg", type: "background", paint: { "background-color": "#DDE3D6" } }];
  if (base === "satellite") {
    sources.base = { type: "raster", tiles: [`${ESRI}/World_Imagery/MapServer/tile/{z}/{y}/{x}`], tileSize: 256, maxzoom: 19, attribution: ATTR.esri };
    layers.push({ id: "base", type: "raster", source: "base" });
    if (labels) {
      sources.labels = { type: "raster", tiles: [`${ESRI}/Reference/World_Boundaries_and_Places/MapServer/tile/{z}/{y}/{x}`], tileSize: 256, maxzoom: 19 };
      layers.push({ id: "labels", type: "raster", source: "labels", paint: { "raster-opacity": 0.9 } });
    }
  } else {
    sources.base = { type: "raster", tiles: ["a", "b", "c"].map((s) => `https://${s}.tile.opentopomap.org/{z}/{x}/{y}.png`), tileSize: 256, maxzoom: 17, attribution: ATTR.topo };
    layers.push({ id: "base", type: "raster", source: "base" });
  }
  return { version: 8, sources, layers };
}

const FALLBACK: StyleSpecification = { version: 8, sources: {}, layers: [{ id: "bg", type: "background", paint: { "background-color": "#DDE3D6" } }] };

export class MapEngine {
  readonly map: MlMap;
  private pins = new Map<string, Marker>();
  private clusters: Marker[] = [];
  private stationPins = new Map<string, Marker>();
  private meMarker: Marker | null = null;
  private ghost: Marker | null = null;
  private data: MapData | null = null;
  private layers: Layers | null = null;
  private base: BaseMap = "satellite";
  private tilt = false;
  private styleKey = "";
  private streetCache: StyleSpecification | null = null;
  private labelled = false;
  private resizer: ResizeObserver;

  private opts: EngineOpts;

  constructor(el: HTMLElement, opts: EngineOpts, view?: { center: [number, number]; zoom: number }) {
    this.opts = opts;
    this.map = new MlMap({
      container: el, style: FALLBACK, center: view?.center ?? [28.0546, 37.6107], zoom: view?.zoom ?? 14,
      maxZoom: 20, maxPitch: 75, attributionControl: false, fadeDuration: 150, dragRotate: true, touchPitch: true,
      refreshExpiredTiles: false,
    });
    this.map.addControl(new AttributionControl({ compact: true, customAttribution: ATTR.dem }), "bottom-right");
    // Attribution stays one tap away (the (i) button), not open over the map.
    const fold = () => el.querySelector(".maplibregl-ctrl-attrib")?.classList.remove("maplibregl-compact-show");
    this.map.on("load", fold);
    this.map.on("styledata", fold);
    this.map.on("rotate", () => this.opts.onView?.(this.map.getBearing(), this.map.getPitch()));
    this.map.on("pitch", () => this.opts.onView?.(this.map.getBearing(), this.map.getPitch()));
    this.resizer = new ResizeObserver(() => this.map.resize());
    this.resizer.observe(el);
    this.map.on("style.load", () => this.addOverlays());
    this.map.on("moveend", () => this.recluster());
    this.map.on("zoomend", () => { this.recluster(); this.opts.onZoom?.(this.map.getZoom()); });
    this.map.on("click", (e) => {
      const t = e.originalEvent.target as HTMLElement;
      if (!t.closest(".pin, .cluster")) this.opts.onSelect(null);
    });
  }

  destroy(): void { this.resizer.disconnect(); this.map.remove(); }

  // ------------------------------------------------------------- styling --
  async setBase(base: BaseMap, layers: Layers): Promise<void> {
    const key = `${base}|${base === "satellite" && layers.labels}`;
    this.base = base;
    this.layers = layers;
    if (key === this.styleKey) { this.applyVisibility(); return; }
    this.styleKey = key;
    if (base !== "street") { this.map.setStyle(rasterStyle(base, layers.labels), { diff: false }); this.opts.onOffline(false); return; }
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
    this.map.setTerrain(this.tilt ? { source: "dem", exaggeration: 1.35 } : null);
  }

  private addOverlays(): void {
    const m = this.map;
    if (!m.getSource("dem")) m.addSource("dem", { type: "raster-dem", tiles: [DEM], tileSize: 256, maxzoom: 15, encoding: "terrarium" });
    if (!m.getSource("dem-shade")) m.addSource("dem-shade", { type: "raster-dem", tiles: [DEM], tileSize: 256, maxzoom: 15, encoding: "terrarium" });
    const empty = fc([]);
    for (const id of ["o-coverage", "o-heat", "o-trail", "o-trail-pts", "o-me"])
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
    add({ id: "o-trail-casing", type: "line", source: "o-trail", filter: ["==", ["get", "gap"], false], layout: { "line-cap": "round", "line-join": "round" },
      paint: { "line-color": "#FFFFFF", "line-width": 6, "line-opacity": ["case", ["get", "past"], 0.85, 0.3] } });
    add({ id: "o-trail", type: "line", source: "o-trail", filter: ["==", ["get", "gap"], false], layout: { "line-cap": "round", "line-join": "round" },
      paint: { "line-color": ["get", "color"], "line-width": 3.5, "line-opacity": ["case", ["get", "past"], 1, 0.3] } });
    add({ id: "o-trail-gap", type: "line", source: "o-trail", filter: ["==", ["get", "gap"], true],
      paint: { "line-color": "#FFFFFF", "line-width": 2.5, "line-dasharray": [1.5, 2], "line-opacity": ["case", ["get", "past"], 0.9, 0.3] } });
    add({ id: "o-trail-pts", type: "circle", source: "o-trail-pts", minzoom: 14,
      paint: { "circle-radius": 3, "circle-color": ["get", "color"], "circle-stroke-color": "#FFFFFF", "circle-stroke-width": 1.5,
        "circle-opacity": ["case", ["get", "past"], 1, 0.3], "circle-stroke-opacity": ["case", ["get", "past"], 1, 0.3] } });
    add({ id: "o-me", type: "fill", source: "o-me", paint: { "fill-color": "#2F6FD0", "fill-opacity": 0.14, "fill-outline-color": "#2F6FD0" } });
    try {
      m.setSky({ "sky-color": "#9CC7EE", "horizon-color": "#E6EEF2", "fog-color": "#E6EEF2",
        "sky-horizon-blend": 0.6, "horizon-fog-blend": 0.5, "fog-ground-blend": 0.4 });
    } catch { /* the sky is decoration */ }
    this.applyTerrain();
    this.applyVisibility();
    if (this.data) this.pushData(this.data, true);
  }

  private applyVisibility(): void {
    const l = this.layers, m = this.map;
    if (!l || !m.getLayer("o-heat")) return;
    const vis = (id: string, on: boolean) => { if (m.getLayer(id)) m.setLayoutProperty(id, "visibility", on ? "visible" : "none"); };
    vis("o-relief", l.relief && this.base !== "street");
    vis("o-coverage", l.coverage);
    vis("o-heat", l.heat);
    for (const id of ["o-trail-casing", "o-trail", "o-trail-gap", "o-trail-pts"]) vis(id, l.trail);
    vis("o-me", l.me);
    for (const mk of this.pins.values()) mk.getElement().style.display = l.animals ? "" : "none";
    for (const mk of this.clusters) mk.getElement().style.display = l.animals ? "" : "none";
    for (const mk of this.stationPins.values()) mk.getElement().style.display = l.stations ? "" : "none";
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
    this.recluster();
  }

  // Only sources whose input changed are re-sent (playback ticks often).
  private sent: Partial<Record<string, unknown>> = {};
  private pushData(d: MapData, force = false): void {
    const set = (id: string, key: unknown, make: () => FeatureCollection) => {
      if (!force && this.sent[id] === key) return;
      const src = this.map.getSource(id) as GeoJSONSource | undefined;
      if (!src) return;
      this.sent[id] = key;
      void src.setData(make());
    };
    set("o-coverage", d.coverage, () => fc(d.coverage.map((c) => ({ type: "Feature", properties: { cls: c.cls }, geometry: { type: "Polygon", coordinates: [c.ring] } }))));
    // Weight so the total stays comparable whether 50 or 50 000 records are shown.
    const w = Math.max(0.015, Math.min(1, 120 / Math.max(1, d.heat.length)));
    set("o-heat", d.heat, () => fc(d.heat.map((p) => ({ type: "Feature", properties: { w }, geometry: { type: "Point", coordinates: [p.lon, p.lat] } }))));
    const trailKey = d.trail ? `${d.selected}:${d.trail.points.length}:${d.trail.points[0]?.time}:${d.trail.color}:${d.cursor === null ? "" : Math.floor(d.cursor / 60_000)}` : "none";
    set("o-trail", trailKey, () => fc(d.trail ? trailFeatures(d.trail.points, d.trail.color, d.cursor) : []));
    set("o-trail-pts", trailKey, () => fc(d.trail ? d.trail.points.map((p) => ({
      type: "Feature", properties: { color: d.trail!.color, past: d.cursor === null || epochMs(p.time) <= d.cursor },
      geometry: { type: "Point", coordinates: [p.lon, p.lat] },
    })) : []));
    set("o-me", d.me, () => fc(d.me ? [{ type: "Feature", properties: {}, geometry: { type: "Polygon", coordinates: [circlePolygon(d.me, Math.max(5, d.me.accuracy))] } }] : []));
  }

  private syncPins(d: MapData): void {
    const label = this.labelled;
    const keep = new Set<string>();
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
      if (el.dataset.html !== html) { el.innerHTML = html; el.dataset.html = html; }
      el.classList.toggle("selected", a.id === d.selected);
      el.style.zIndex = a.id === d.selected ? "5" : String(Math.min(4, a.attention));
      el.style.display = this.layers?.animals === false ? "none" : "";
      mk.setLngLat([a.last.lon, a.last.lat]);
    }
    for (const [id, mk] of this.pins) if (!keep.has(id)) { mk.remove(); this.pins.delete(id); }
  }

  /** Overlapping pins merge into a count bubble until zoomed in. */
  private recluster(): void {
    const d = this.data;
    for (const c of this.clusters) c.remove();
    this.clusters = [];
    if (!d) return;
    const zoom = this.map.getZoom();
    if ((zoom >= LABEL_ZOOM) !== this.labelled) { this.labelled = zoom >= LABEL_ZOOM; this.syncPins(d); }
    const pts = d.herd.filter((a) => a.last).map((a) => ({ a, p: this.map.project([a.last!.lon, a.last!.lat]) }));
    for (const mk of this.pins.values()) mk.getElement().classList.remove("in-cluster");
    if (zoom >= 17.5) return;
    const used = new Set<string>();
    for (const { a, p } of pts) {
      if (used.has(a.id) || a.id === d.selected) continue;
      const group = pts.filter((o) => !used.has(o.a.id) && o.a.id !== d.selected && Math.hypot(o.p.x - p.x, o.p.y - p.y) < 36);
      if (group.length < 2) continue;
      for (const g of group) { used.add(g.a.id); this.pins.get(g.a.id)?.getElement().classList.add("in-cluster"); }
      const worst = group.reduce((w, g) => (g.a.attention > w.attention ? g.a : w), group[0].a);
      const lat = group.reduce((s, g) => s + g.a.last!.lat, 0) / group.length;
      const lon = group.reduce((s, g) => s + g.a.last!.lon, 0) / group.length;
      const el = document.createElement("button");
      el.className = `cluster c-${toneOfStatus(worst.status)}`;
      el.innerHTML = `<b>${group.length}</b>`;
      el.setAttribute("aria-label", `${group.length} hayvan`);
      el.style.display = this.layers?.animals === false ? "none" : "";
      const members = group.map((g) => g.a.last!);
      el.addEventListener("click", (e) => {
        e.stopPropagation();
        const b = new LngLatBounds();
        for (const m of members) b.extend([m.lon, m.lat]);
        this.map.fitBounds(b, { padding: 90, maxZoom: 18.5, duration: 600 });
      });
      this.clusters.push(new Marker({ element: el, anchor: "center" }).setLngLat([lon, lat]).addTo(this.map));
    }
  }

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
      mk.getElement().style.display = this.layers?.stations === false ? "none" : "";
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
  setPadding(bottom: number, top = 140): void {
    this.map.setPadding({ top, bottom, left: 32, right: 88 });
    this.map.getContainer().style.setProperty("--sheet-h", `${bottom}px`);
  }

  fit(points: LatLon[], maxZoom = 17): void {
    if (!points.length) return;
    if (points.length === 1) { this.map.easeTo({ center: [points[0].lon, points[0].lat], zoom: Math.max(this.map.getZoom(), 16), duration: 600 }); return; }
    const b = new LngLatBounds();
    for (const p of points) b.extend([p.lon, p.lat]);
    this.map.fitBounds(b, { padding: 48, maxZoom, duration: 700 });
  }

  flyTo(p: LatLon, zoom?: number): void {
    this.map.easeTo({ center: [p.lon, p.lat], zoom: zoom ?? Math.max(this.map.getZoom(), 16.5), duration: 600 });
  }

  view(): { center: [number, number]; zoom: number } {
    const c = this.map.getCenter();
    return { center: [c.lng, c.lat], zoom: this.map.getZoom() };
  }
}
