// ORUN Alanlar package 2: editable LOCAL pasture drafts.
// Derived from ORUN-Saha.html and ORUN-Mera.html. No tracker write/ACK path.
import { useCallback, useEffect, useRef, useState, type ChangeEvent } from "react";
import { Map as MlMap, Marker, type GeoJSONSource, type StyleSpecification } from "maplibre-gl";
import type { Feature, FeatureCollection } from "geojson";
import { ArrowCounterClockwise, ArrowClockwise, Check, DownloadSimple, MapTrifold, Plus, Polygon, Trash, UploadSimple, X } from "@phosphor-icons/react";
import { useBack } from "../../app/nav.ts";
import { useApp } from "../../app/store.ts";
import { DemoFlag, Header } from "../../ds/components.tsx";
import { areaHa, asGeoJSON, canonicalRing, importGeoJSON, loadZones, perimeterM, persistZones, validateRing, validateSet, type Point, type Ring, type Zone } from "./geometry.ts";
import "./zones.css";

function hectares(n: number): string { return n.toLocaleString("tr-TR", { minimumFractionDigits: 2, maximumFractionDigits: 2 }); }
function metres(n: number): string { return n > 1000 ? `${(n / 1000).toFixed(2)} km` : `${Math.round(n)} m`; }
const featureCollection = (features: Feature[]): FeatureCollection => ({ type: "FeatureCollection", features });
const ringFeature = (ring: Ring): Feature => ({
  type: "Feature", properties: {},
  geometry: { type: "Polygon", coordinates: [[...ring, ring[0]]] },
});
const lineFeature = (ring: Ring): Feature => ({
  type: "Feature", properties: {},
  geometry: { type: "LineString", coordinates: ring },
});
const SAT_STYLE: StyleSpecification = {
  version: 8,
  sources: {
    satellite: {
      type: "raster",
      tiles: ["https://server.arcgisonline.com/ArcGIS/rest/services/World_Imagery/MapServer/tile/{z}/{y}/{x}"],
      tileSize: 256, maxzoom: 19, attribution: "© Esri, Maxar, Earthstar Geographics",
    },
  },
  layers: [
    { id: "background", type: "background", paint: { "background-color": "#DCE8D5" } },
    { id: "satellite", type: "raster", source: "satellite" },
  ],
};

function ZoneEditor({ zone, all, onCancel, onSave }: {
  zone: Zone | null; all: Zone[]; onCancel: () => void; onSave: (zone: Zone) => void;
}) {
  const [name, setName] = useState(zone?.name ?? `Mera ${all.length + 1}`);
  const [active, setActive] = useState(zone?.active ?? true);
  const [draft, setDraft] = useState<Ring>(() => zone?.ring.map((p) => [...p] as Point) ?? []);
  const [past, setPast] = useState<Ring[]>([]);
  const [future, setFuture] = useState<Ring[]>([]);
  const [mode, setMode] = useState<"cross" | "tap">("cross");
  const [error, setError] = useState("");
  const host = useRef<HTMLDivElement>(null);
  const map = useRef<MlMap | null>(null);
  const markers = useRef<Marker[]>([]);
  const draftNow = useRef(draft);
  const modeNow = useRef(mode);
  draftNow.current = draft; modeNow.current = mode;

  const changeDraft = useCallback((next: Ring) => {
    const prev = draftNow.current;
    setPast((v) => [...v.slice(-59), prev]);
    setFuture([]);
    setDraft(next);
    setError("");
  }, []);
  const changeRef = useRef(changeDraft);
  changeRef.current = changeDraft;

  const renderRef = useRef<() => void>(() => undefined);
  renderRef.current = () => {
    const m = map.current;
    if (!m || !m.getSource("draft")) return;
    const p = draftNow.current;
    const saved = all.filter((z) => z.id !== zone?.id).map((z) => ringFeature(z.ring));
    (m.getSource("saved") as GeoJSONSource).setData(featureCollection(saved));
    (m.getSource("draft") as GeoJSONSource).setData(
      featureCollection(p.length >= 3 ? [ringFeature(p)] : p.length >= 2 ? [lineFeature(p)] : []),
    );
    for (const marker of markers.current) marker.remove();
    markers.current = [];
    p.forEach(([lon, lat], i) => {
      const handle = document.createElement("button");
      handle.type = "button";
      handle.className = "zone-vertex";
      handle.setAttribute("aria-label", `${i + 1}. köşeyi taşı`);
      handle.textContent = String(i + 1);
      handle.addEventListener("click", (e) => e.stopPropagation());
      const marker = new Marker({ element: handle, draggable: true, anchor: "center" })
        .setLngLat([lon, lat]).addTo(m);
      marker.on("dragend", () => {
        const v = marker.getLngLat(), next = [...draftNow.current];
        if (!next[i]) return;
        next[i] = [v.lng, v.lat];
        changeRef.current(next);
      });
      markers.current.push(marker);
    });
    // A small + at each edge midpoint allows insertion without starting over.
    if (p.length >= 2) p.forEach((a, i) => {
      if (p.length === 2 && i === 1) return;
      const b = p[(i + 1) % p.length];
      const el = document.createElement("button");
      el.type = "button";
      el.className = "zone-midpoint";
      el.textContent = "+";
      el.setAttribute("aria-label", `${i + 1}. kenara köşe ekle`);
      el.addEventListener("click", (e) => {
        e.stopPropagation();
        if (draftNow.current.length >= 64) return;
        const next = [...draftNow.current];
        next.splice(i + 1, 0, [(a[0] + b[0]) / 2, (a[1] + b[1]) / 2]);
        changeRef.current(next);
      });
      markers.current.push(new Marker({ element: el, anchor: "center" })
        .setLngLat([(a[0] + b[0]) / 2, (a[1] + b[1]) / 2]).addTo(m));
    });
  };

  useEffect(() => {
    if (!host.current) return;
    const initial = zone?.ring ?? all[0]?.ring ?? [];
    const center: [number, number] = initial[0] ?? [28.0546, 37.6107];
    const m = new MlMap({
      container: host.current, style: SAT_STYLE, center, zoom: initial.length ? 15 : 13,
      maxZoom: 20, attributionControl: true, dragRotate: false, touchPitch: false,
    });
    map.current = m;
    m.on("load", () => {
      m.addSource("saved", { type: "geojson", data: featureCollection([]) });
      m.addSource("draft", { type: "geojson", data: featureCollection([]) });
      m.addLayer({ id: "zone-saved-fill", type: "fill", source: "saved",
        paint: { "fill-color": "#FFD60A", "fill-opacity": 0.14 } });
      m.addLayer({ id: "zone-saved-line", type: "line", source: "saved",
        paint: { "line-color": "#FFD60A", "line-width": 2 } });
      m.addLayer({ id: "zone-draft-fill", type: "fill", source: "draft",
        paint: { "fill-color": "#39D990", "fill-opacity": 0.19 } });
      m.addLayer({ id: "zone-draft-line", type: "line", source: "draft",
        paint: { "line-color": "#39D990", "line-width": 3 } });
      renderRef.current();
      if (initial.length >= 3) {
        const xs = initial.map((p) => p[0]), ys = initial.map((p) => p[1]);
        m.fitBounds([[Math.min(...xs), Math.min(...ys)], [Math.max(...xs), Math.max(...ys)]],
          { padding: 55, duration: 0, maxZoom: 17 });
      }
    });
    m.on("click", (e) => {
      if (modeNow.current !== "tap" || draftNow.current.length >= 64) return;
      changeRef.current([...draftNow.current, [e.lngLat.lng, e.lngLat.lat]]);
    });
    return () => { markers.current.forEach((p) => p.remove()); markers.current = []; m.remove(); map.current = null; };
    // Only construct this map once per mounted editor; all/draft are read via refs.
  }, []); // eslint-disable-line react-hooks/exhaustive-deps
  useEffect(() => { renderRef.current(); }, [draft, all]); // eslint-disable-line react-hooks/exhaustive-deps

  useBack(onCancel);
  const undo = () => {
    if (!past.length) return;
    setFuture((v) => [...v, draft]); setDraft(past[past.length - 1]); setPast(past.slice(0, -1)); setError("");
  };
  const redo = () => {
    if (!future.length) return;
    setPast((v) => [...v, draft]); setDraft(future[future.length - 1]); setFuture(future.slice(0, -1)); setError("");
  };
  const addCross = () => {
    if (!map.current || draft.length >= 64) return;
    const p = map.current.getCenter(); changeDraft([...draft, [p.lng, p.lat]]);
  };
  const valid = draft.length >= 3 ? validateRing(draft) : "En az 3 köşe gerekli.";
  const save = () => {
    if (valid) { setError(valid); return; }
    const cleanName = name.trim();
    if (!cleanName) { setError("Alan adı gerekli."); return; }
    const candidate = { id: zone?.id ?? crypto.randomUUID(), name: cleanName, active,
      ring: canonicalRing(draft) };
    const allNext = [...all.filter((x) => x.id !== zone?.id), candidate];
    const issue = validateSet(allNext);
    if (issue) { setError(issue); return; }
    onSave(candidate);
  };

  return (
    <div className="zone-editor">
      <div ref={host} className="zone-map" />
      <div className="zone-top">
        <button className="zone-round" onClick={onCancel} aria-label="Vazgeç"><X size={22} /></button>
        <div className="zone-heading"><b>{zone ? "Sınırı düzenle" : "Yeni mera"}</b><span>Yalnız bu telefonda taslak</span></div>
        <button className="btn primary small" disabled={Boolean(valid)} onClick={save}><Check size={18} /> Kaydet</button>
      </div>
      {mode === "cross" && <div className="zone-crosshair" aria-hidden="true">+</div>}
      <div className="zone-draw-box">
        <div className="zone-metrics">
          <span><b>{hectares(areaHa(draft))}</b> ha · {hectares(areaHa(draft) * 10)} dekar</span>
          <span><b>{draft.length}</b> / 64 köşe · {metres(perimeterM(draft))}</span>
        </div>
        <label className="zone-label" htmlFor="zone-name">Mera adı
          <input id="zone-name" value={name} maxLength={40} onChange={(e) => setName(e.target.value)} />
        </label>
        <label className="zone-check"><input type="checkbox" checked={active} onChange={(e) => setActive(e.target.checked)} />
          Hazırlanan sınıra dahil et (cihaza göndermez)
        </label>
        <div className="zone-actions">
          <button className="btn small" onClick={() => setMode(mode === "cross" ? "tap" : "cross")}>
            {mode === "cross" ? "Nişangâh" : "Dokunarak"}
          </button>
          {mode === "cross" && <button className="btn signal small" onClick={addCross}><Plus size={18} /> Köşe ekle</button>}
          <button className="zone-icon-btn" onClick={undo} disabled={!past.length} aria-label="Geri al"><ArrowCounterClockwise size={21} /></button>
          <button className="zone-icon-btn" onClick={redo} disabled={!future.length} aria-label="Yinele"><ArrowClockwise size={21} /></button>
        </div>
        <p className={error || (draft.length >= 3 && valid) ? "zone-error" : "zone-hint"}>
          {error || valid || (mode === "cross" ? "Haritayı kaydır, artının altına köşe ekle. Numaralı köşeleri sürükleyebilirsin." :
            "Haritaya dokunarak köşe ekle. Köşeleri sürükleyebilirsin.")}
        </p>
      </div>
    </div>
  );
}

export function ZonesScreen() {
  const m = useApp();
  const [zones, setZones] = useState<Zone[]>(loadZones);
  const [editId, setEditId] = useState<string | null>(null);
  const [editing, setEditing] = useState(false);
  const [message, setMessage] = useState("");
  const file = useRef<HTMLInputElement>(null);
  const selected = zones.filter((z) => z.active);
  const totalVertices = selected.reduce((n, z) => n + z.ring.length, 0);

  const commit = (next: Zone[]): boolean => {
    try { persistZones(next); setZones(next); setMessage(""); return true; }
    catch (e) { setMessage(e instanceof Error ? e.message : String(e)); return false; }
  };
  const start = (z: Zone | null) => { setEditId(z?.id ?? null); setEditing(true); setMessage(""); };
  const save = (zone: Zone) => {
    const next = editId === null ? [...zones, zone] : zones.map((z) => z.id === editId ? zone : z);
    if (commit(next)) { setEditing(false); setMessage("Alan telefona taslak olarak kaydedildi; tasmaya gönderilmedi."); }
  };
  const remove = (z: Zone) => {
    if (window.confirm(`${z.name} taslağı bu telefondan silinsin mi? Tasmadaki sınır değişmez.`))
      if (commit(zones.filter((v) => v.id !== z.id))) setMessage("Yerel taslak silindi; tasmadaki sınır değişmedi.");
  };
  const download = () => {
    const url = URL.createObjectURL(new Blob([JSON.stringify(asGeoJSON(zones), null, 2)], { type: "application/geo+json" }));
    const a = document.createElement("a"); a.href = url; a.download = "orun-mera-taslaklari.geojson";
    document.body.appendChild(a); a.click();
    setTimeout(() => { URL.revokeObjectURL(url); a.remove(); }, 1000);
  };
  const readFile = async (e: ChangeEvent<HTMLInputElement>) => {
    const input = e.target.files?.[0]; e.target.value = "";
    if (!input) return;
    try {
      if (input.size > 1_000_000) throw new Error("GeoJSON dosyası 1 MB altında olmalı.");
      const imported = importGeoJSON(await input.text());
      if (commit([...zones, ...imported])) setMessage(`${imported.length} alan yerel taslaklara eklendi; tasmaya gönderilmedi.`);
    } catch (err) { setMessage(err instanceof Error ? err.message : String(err)); }
  };

  if (editing) return <ZoneEditor zone={zones.find((z) => z.id === editId) ?? null} all={zones}
    onCancel={() => setEditing(false)} onSave={save} />;

  return (
    <div className="page zones-page">
      <Header title="Alanlar"><button className="btn primary small" onClick={() => start(null)}><Plus size={18} /> Yeni alan</button></Header>
      {m.demo && <DemoFlag onPage />}
      <div className="zone-intro">
        <p>Haritada meraları çiz, ölç ve kaydet. <b>Bu alanlar yerel taslaktır;</b> tasmada henüz uygulanmadı.</p>
        <div className="zone-capacity"><b>{selected.length}/8 alan</b><b>{totalVertices}/64 köşe</b></div>
        <div className="zone-meter"><i style={{ width: `${Math.min(100, totalVertices / 64 * 100)}%` }} /></div>
      </div>
      {message && <div className="zone-message" role="status">{message}</div>}
      {zones.length === 0 ? (
        <div className="zone-empty">
          <Polygon size={46} /><h2>Henüz mera sınırı yok</h2>
          <p>Haritada sınır çizmek için yeni alan oluştur. Haritaya dokunabilir veya nişangâhla köşe ekleyebilirsin.</p>
          <button className="btn primary wide" onClick={() => start(null)}><MapTrifold size={20} /> Haritada alan çiz</button>
        </div>
      ) : zones.map((z) => (
        <div className="zone-card" key={z.id}>
          <div className="zone-card-head">
            <span className="zone-color" /><div><b>{z.name}</b><small>Yerel taslak · {z.active ? "Sınır listesine dahil" : "Hariç"}</small></div>
            <label className="zone-active" title="Hazırlanan sınıra dahil et"><input type="checkbox" checked={z.active}
              onChange={() => commit(zones.map((v) => v.id === z.id ? { ...v, active: !v.active } : v))} /> Dahil</label>
          </div>
          <div className="zone-card-stats"><span>{hectares(areaHa(z.ring))} ha</span><span>{z.ring.length} köşe</span><span>{metres(perimeterM(z.ring))}</span></div>
          <div className="zone-card-actions">
            <button className="btn small" onClick={() => start(z)}><MapTrifold size={17} /> Haritada düzenle</button>
            <button className="btn small danger" onClick={() => remove(z)} aria-label={`${z.name} taslağını sil`}><Trash size={18} /></button>
          </div>
        </div>
      ))}
      <div className="zone-import-row">
        <input ref={file} type="file" accept=".geojson,.json,application/geo+json" hidden onChange={(e) => { void readFile(e); }} />
        <button className="btn small" onClick={() => file.current?.click()}><UploadSimple size={18} /> GeoJSON içe al</button>
        <button className="btn small" disabled={!zones.length} onClick={download}><DownloadSimple size={18} /> Dışa aktar</button>
      </div>
      <p className="zone-footer-note">Alanlar ve köşeler yalnız telefonda saklanır. Tasmaya alan uygulama, teslim sonucu ve senkronizasyon sonraki ayrı aşamadır.</p>
    </div>
  );
}
