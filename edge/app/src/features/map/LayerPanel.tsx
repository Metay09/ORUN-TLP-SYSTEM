import type { ReactNode } from "react";
import {
  CellSignalHigh, Cow, Crosshair, Cube, Fire, Footprints, LineSegments, Mountains, Path, Polygon, Tag, WifiHigh,
} from "@phosphor-icons/react";
import { tr } from "../../app/i18n/tr.ts";
import { useApp } from "../../app/store.ts";
import { DEFAULT_SAT, type BaseMap, type Layers, type Range, type ZoneFill } from "../../app/stores.ts";
import { Chips, Dialog, Section } from "../../ds/components.tsx";

const BASES: BaseMap[] = ["satellite", "terrain", "street"];
const RANGES: Range[] = ["today", "h24", "d3", "d7", "d30"];
const FILLS: ZoneFill[] = ["none", "light", "hatch"];
// Esri Wayback releases (id: imagery date). Older releases are sometimes
// sharper over a given pasture than the current mosaic.
const WAYBACK: [string, string][] = [
  ["26334", "2026-08-05"], ["22869", "2026-03-25"], ["13192", "2025-12-18"], ["48925", "2025-06-26"], ["36557", "2025-01-30"],
  ["39767", "2024-06-27"], ["41468", "2024-01-18"], ["56102", "2023-12-07"], ["47963", "2023-06-29"], ["11475", "2023-01-11"],
  ["4905", "2022-06-29"], ["42663", "2022-01-12"], ["48376", "2021-06-09"], ["18289", "2020-07-01"], ["6036", "2019-01-09"],
];

type Tile = { key: keyof Layers; label: string; icon: ReactNode };
const TILES: Tile[] = [
  { key: "animals", label: tr.map.layer.animals, icon: <Cow size={24} /> },
  { key: "zones", label: tr.map.layer.zones, icon: <Polygon size={24} /> },
  { key: "trail", label: tr.map.layer.trail, icon: <Path size={24} /> },
  { key: "herdTrails", label: "Sürü izleri", icon: <Footprints size={24} /> },
  { key: "heat", label: tr.map.layer.heat, icon: <Fire size={24} /> },
  { key: "coverage", label: tr.map.layer.coverage, icon: <CellSignalHigh size={24} /> },
  { key: "stations", label: tr.map.layer.stations, icon: <WifiHigh size={24} /> },
  { key: "links", label: "Bağlantı çizgisi", icon: <LineSegments size={24} /> },
  { key: "me", label: tr.map.layer.me, icon: <Crosshair size={24} /> },
];

// Harita katmanları (sahip isteği 2026-11-11: düzenle). Üstte harita türü ve
// görünüm (3B, rölyef), sonra haritada ne görüneceği karo düğmelerle; açık
// katmanın ayarı ya da lejantı altında; dönem; uydu ayrıntıları kapalı.
export function LayerPanel({ onClose }: { onClose: () => void }) {
  const m = useApp();
  const p = m.prefs, L = p.layers;
  const set = (k: keyof Layers, v: boolean) => m.actions.setPrefs({ layers: { ...L, [k]: v } });
  return (
    <Dialog title={tr.map.layers} onClose={onClose}>
      <Section title={tr.map.base.title} />
      <div className="bases">
        {BASES.map((b) => (
          <button key={b} className="base-opt" aria-pressed={p.base === b} onClick={() => m.actions.setPrefs({ base: b })}>
            <span className={`thumb ${b}`} />{tr.map.base[b]}
          </button>
        ))}
      </div>
      <div className="lay-view">
        <button className="chip" aria-pressed={p.tilt} onClick={() => m.actions.setPrefs({ tilt: !p.tilt })}><Cube size={18} />3B</button>
        {p.base !== "street" && <button className="chip" aria-pressed={L.relief} onClick={() => set("relief", !L.relief)}><Mountains size={18} />Rölyef</button>}
        {p.base === "satellite" && <button className="chip" aria-pressed={L.labels} onClick={() => set("labels", !L.labels)}><Tag size={18} />Yer adları</button>}
      </div>

      <Section title="Haritada göster"
        info={`İz: ${tr.map.layerHelp.trail}. Sürü izleri: bütün hayvanların izi, yalnız incelemek için. Yoğunluk: ${tr.map.layerHelp.heat}. Kapsama: ${tr.map.layerHelp.coverage}. Bağlantı çizgisi: ${tr.map.layerHelp.links}.`} />
      <div className="lay-grid">
        {TILES.map((t) => (
          <button key={t.key} className="lay-tile" aria-pressed={L[t.key]} onClick={() => set(t.key, !L[t.key])}>
            {t.icon}<span>{t.label}</span>
          </button>
        ))}
      </div>
      {L.zones && (
        <div className="lay-opt">
          <span className="sm">{tr.map.zoneFill.title}</span>
          <Chips value={p.zoneFill} onChange={(zoneFill) => m.actions.setPrefs({ zoneFill })} options={FILLS.map((f) => ({ value: f, label: tr.map.zoneFill[f] }))} />
        </div>
      )}
      {L.heat && <div className="lay-opt"><span className="sm">{tr.map.layer.heat}</span><div className="heat-ramp" style={{ margin: 0 }} /><div className="legend" style={{ justifyContent: "space-between", padding: 0 }}><span>Az</span><span>Çok</span></div></div>}
      {L.coverage && (
        <div className="lay-opt">
          <span className="sm">{tr.map.layer.coverage}</span>
          <div className="legend" style={{ padding: 0 }}>
            <span><i style={{ background: "#2E9E5B" }} />{tr.map.coverageLegend.strong}</span>
            <span><i style={{ background: "#E2A21C" }} />{tr.map.coverageLegend.medium}</span>
            <span><i style={{ background: "#D9472F" }} />{tr.map.coverageLegend.weak}</span>
            <span><i style={{ background: "#6B7280" }} />{tr.map.coverageLegend.none}</span>
          </div>
        </div>
      )}

      <Section title={tr.map.range.title} info="İz, yoğunluk ve sürü izleri bu dönemin kayıtlarından çizilir." />
      <Chips value={p.range} onChange={(r) => m.actions.setPrefs({ range: r })} options={RANGES.map((r) => ({ value: r, label: tr.map.range[r] }))} />

      {p.base === "satellite" && (
        <details className="tech">
          <summary>Uydu görüntüsü ayarları</summary>
          <div className="stack" style={{ padding: "0 16px 10px" }}>
            <span className="sm strong">{tr.map.sat.title}</span>
            <Chips value={p.sat.kind} onChange={(kind) => m.actions.setPrefs({ sat: { ...p.sat, kind } })}
              options={[{ value: "esri", label: tr.map.sat.esri }, { value: "wayback", label: tr.map.sat.wayback }]} />
            {p.sat.kind === "wayback" && (
              <label className="row" style={{ justifyContent: "space-between" }}>
                <span className="sm">{tr.map.sat.release}</span>
                <select className="inp" style={{ width: "auto" }} value={p.sat.release} onChange={(e) => m.actions.setPrefs({ sat: { ...p.sat, release: e.target.value } })}>
                  {WAYBACK.map(([id, d]) => <option key={id} value={id}>{d}</option>)}
                </select>
              </label>
            )}
            <label className="stack" style={{ gap: 2 }}>
              <span className="row" style={{ justifyContent: "space-between" }}><span className="sm">{tr.map.sat.maxZoom}</span><b>{p.sat.maxZoom}</b></span>
              <input type="range" min={14} max={19} step={1} value={p.sat.maxZoom} style={{ accentColor: "var(--brand)" }}
                onChange={(e) => m.actions.setPrefs({ sat: { ...p.sat, maxZoom: Number(e.target.value) } })} />
              <span className="fine">{tr.map.sat.maxZoomHelp(p.sat.maxZoom)}</span>
              {p.sat.maxZoom !== DEFAULT_SAT.maxZoom && <button className="btn quiet small" style={{ alignSelf: "flex-start" }}
                onClick={() => m.actions.setPrefs({ sat: { ...p.sat, maxZoom: DEFAULT_SAT.maxZoom } })}>{DEFAULT_SAT.maxZoom}'ye dön</button>}
            </label>
          </div>
        </details>
      )}
      <p className="fine" style={{ padding: "4px 20px 0" }}>{tr.map.tilesNote}</p>
    </Dialog>
  );
}
