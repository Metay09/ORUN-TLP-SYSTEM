// Full-screen field map (Leaflet). Animals are round pins in their own colour;
// a pin fades to grey when its position is old. The selected animal shows its
// last 24 h as a calm grey trail. The phone's own position is a blue dot and
// never mixed up with an animal's GNSS position.
import { useEffect, useRef } from "react";
import L from "leaflet";
import "leaflet/dist/leaflet.css";
import type { Animal } from "./herd.ts";
import { COW_PATH } from "./cowPath.ts";

export type Layer = "uydu" | "harita" | "arazi";
export interface MyPosition { lat: number; lon: number; accuracy: number; }

const LAYERS: Record<Layer, { url: string; attribution: string; maxNativeZoom: number }> = {
  uydu: {
    url: "https://server.arcgisonline.com/ArcGIS/rest/services/World_Imagery/MapServer/tile/{z}/{y}/{x}",
    attribution: "Görüntü © Esri, Maxar, Earthstar Geographics", maxNativeZoom: 19,
  },
  harita: {
    url: "https://tile.openstreetmap.org/{z}/{x}/{y}.png",
    attribution: "© OpenStreetMap katkıcıları", maxNativeZoom: 19,
  },
  arazi: {
    url: "https://tile.opentopomap.org/{z}/{x}/{y}.png",
    attribution: "© OpenStreetMap katkıcıları, SRTM · © OpenTopoMap (CC-BY-SA)", maxNativeZoom: 17,
  },
};

const COW_SVG = `<svg viewBox="0 0 256 256" width="22" height="22" fill="currentColor" aria-hidden="true"><path d="${COW_PATH}"/></svg>`;

export function initialsOf(name: string): string {
  const parts = name.trim().split(/\s+/).filter(Boolean);
  const s = parts.length > 1 ? parts[0][0] + parts[1][0] : name.trim().slice(0, 1);
  return s.toLocaleUpperCase("tr-TR");
}

const esc = (s: string) => s.replace(/[&<>"]/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;" })[c]!);

function pinHtml(a: Animal, selected: boolean): string {
  const cls = `pin ${a.freshness}${selected ? " selected" : ""}${a.battery === "critical" ? " alarm" : ""}`;
  return `<div class="${cls}" style="--pin:${a.color}">${a.named ? `<span>${esc(initialsOf(a.name))}</span>` : COW_SVG}</div>`;
}

interface Props {
  herd: Animal[];
  selected: string | null;
  layer: Layer;
  me: MyPosition | null;
  fitKey: number;            // bump to frame the whole herd
  centerMeKey: number;       // bump to centre on the phone
  bottomInset: number;       // px covered by the bottom sheet
  onSelect: (id: string | null) => void;
}

export function MapView({ herd, selected, layer, me, fitKey, centerMeKey, bottomInset, onSelect }: Props) {
  const el = useRef<HTMLDivElement>(null);
  const map = useRef<L.Map | null>(null);
  const tiles = useRef<L.TileLayer | null>(null);
  const pins = useRef(new Map<string, L.Marker>());
  const trail = useRef<L.LayerGroup | null>(null);
  const meLayer = useRef<L.LayerGroup | null>(null);
  const framed = useRef(false);
  const select = useRef(onSelect);
  select.current = onSelect;

  // Create once.
  useEffect(() => {
    if (!el.current) return;
    const m = L.map(el.current, { zoomControl: false, attributionControl: false, center: [37.61, 28.05], zoom: 13 });
    // Top-left, under the status pill: the sheet covers the bottom.
    L.control.attribution({ position: "topleft", prefix: false }).addTo(m);
    m.on("click", () => select.current(null));
    trail.current = L.layerGroup().addTo(m);
    meLayer.current = L.layerGroup().addTo(m);
    map.current = m;
    return () => { m.remove(); map.current = null; pins.current.clear(); };
  }, []);

  // Base layer.
  useEffect(() => {
    const m = map.current;
    if (!m) return;
    tiles.current?.remove();
    const l = LAYERS[layer];
    tiles.current = L.tileLayer(l.url, { attribution: l.attribution, maxZoom: 20, maxNativeZoom: l.maxNativeZoom })
      .addTo(m);
  }, [layer]);

  // Pins.
  useEffect(() => {
    const m = map.current;
    if (!m) return;
    const seen = new Set<string>();
    for (const a of herd) {
      if (!a.last) continue;
      seen.add(a.id);
      const ll: L.LatLngExpression = [a.last.lat, a.last.lon];
      const icon = L.divIcon({ className: "pin-wrap", html: pinHtml(a, a.id === selected), iconSize: [44, 44], iconAnchor: [22, 22] });
      let mk = pins.current.get(a.id);
      if (!mk) {
        mk = L.marker(ll, { icon, keyboard: false, riseOnHover: true });
        mk.on("click", (e) => { L.DomEvent.stopPropagation(e); select.current(a.id); });
        mk.addTo(m);
        pins.current.set(a.id, mk);
      } else {
        mk.setLatLng(ll);
        mk.setIcon(icon);
      }
      mk.setZIndexOffset(a.id === selected ? 1000 : 0);
    }
    for (const [id, mk] of pins.current) if (!seen.has(id)) { mk.remove(); pins.current.delete(id); }
    if (!framed.current && seen.size) { framed.current = true; frame(m, herd, bottomInset); }
  }, [herd, selected, bottomInset]);

  // Trail of the selected animal.
  useEffect(() => {
    const g = trail.current;
    if (!g) return;
    g.clearLayers();
    const a = herd.find((x) => x.id === selected);
    if (!a || a.trail.length < 2) return;
    const line = a.trail.map((p) => [p.lat, p.lon] as L.LatLngTuple);
    L.polyline(line, { color: "#ffffff", weight: 6, opacity: 0.55, interactive: false }).addTo(g);
    L.polyline(line, { color: a.color, weight: 3, opacity: 0.9, dashArray: "1 7", lineCap: "round", interactive: false }).addTo(g);
  }, [herd, selected]);

  // Centre on the selected animal, above the sheet.
  useEffect(() => {
    const m = map.current;
    const a = herd.find((x) => x.id === selected);
    if (!m || !a?.last) return;
    const z = Math.max(m.getZoom(), 15);
    const pt = m.project([a.last.lat, a.last.lon], z).add([0, bottomInset / 2]);
    m.flyTo(m.unproject(pt, z), z, { duration: 0.5 });
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [selected]);

  // Phone position.
  useEffect(() => {
    const g = meLayer.current;
    if (!g) return;
    g.clearLayers();
    if (!me) return;
    L.circle([me.lat, me.lon], { radius: me.accuracy, color: "#2F6F8F", weight: 1, opacity: 0.4, fillOpacity: 0.08, interactive: false }).addTo(g);
    L.circleMarker([me.lat, me.lon], { radius: 7, color: "#ffffff", weight: 3, fillColor: "#2F6F8F", fillOpacity: 1, interactive: false }).addTo(g);
  }, [me]);

  useEffect(() => { if (map.current && fitKey) frame(map.current, herd, bottomInset); },
    // eslint-disable-next-line react-hooks/exhaustive-deps
    [fitKey]);
  useEffect(() => {
    if (map.current && centerMeKey && me) map.current.flyTo([me.lat, me.lon], Math.max(map.current.getZoom(), 15), { duration: 0.5 });
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [centerMeKey]);

  return <div ref={el} className="map-canvas" />;
}

function frame(m: L.Map, herd: Animal[], bottomInset: number): void {
  const ll = herd.filter((a) => a.last).map((a) => [a.last!.lat, a.last!.lon] as L.LatLngTuple);
  if (!ll.length) return;
  if (ll.length === 1) { m.setView(ll[0], 16); return; }
  m.fitBounds(L.latLngBounds(ll), { paddingTopLeft: [40, 90], paddingBottomRight: [40, bottomInset + 40], maxZoom: 17 });
}
