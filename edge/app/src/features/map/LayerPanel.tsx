// Map type, 3D and layer switches, the period, and the legends.
import { tr } from "../../app/i18n/tr.ts";
import { useApp } from "../../app/store.ts";
import { DEFAULT_SAT, type BaseMap, type Layers, type Range, type ZoneFill } from "../../app/stores.ts";
import { Chips, Dialog, Section, ToggleRow } from "../../ds/components.tsx";

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
      <ToggleRow title={tr.map.tilt} on={p.tilt} onChange={(v) => m.actions.setPrefs({ tilt: v })} />
      {p.base === "satellite" && <ToggleRow title={tr.map.layer.labels} on={L.labels} onChange={(v) => set("labels", v)} />}
      {p.base === "satellite" && (
        <div className="stack" style={{ padding: "6px 16px 10px" }}>
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
      )}
      {p.base !== "street" && <ToggleRow title={tr.map.layer.relief} on={L.relief} onChange={(v) => set("relief", v)} />}

      <Section title={tr.map.layer.title} />
      <ToggleRow title={tr.map.layer.animals} on={L.animals} onChange={(v) => set("animals", v)} />
      <ToggleRow title={tr.map.layer.zones} sub={tr.map.layerHelp.zones} on={L.zones} onChange={(v) => set("zones", v)} />
      {L.zones && (
        <div className="stack" style={{ padding: "0 16px 8px" }}>
          <span className="sm">{tr.map.zoneFill.title}</span>
          <Chips value={p.zoneFill} onChange={(zoneFill) => m.actions.setPrefs({ zoneFill })} options={FILLS.map((f) => ({ value: f, label: tr.map.zoneFill[f] }))} />
        </div>
      )}
      <ToggleRow title={tr.map.layer.trail} sub={tr.map.layerHelp.trail} on={L.trail} onChange={(v) => set("trail", v)} />
      <ToggleRow title={tr.map.layer.herdTrails} sub={tr.map.layerHelp.herdTrails} on={L.herdTrails} onChange={(v) => set("herdTrails", v)} />
      <ToggleRow title={tr.map.layer.heat} sub={tr.map.layerHelp.heat} on={L.heat} onChange={(v) => set("heat", v)} />
      {L.heat && <><div className="heat-ramp" /><div className="legend" style={{ justifyContent: "space-between", paddingTop: 4 }}><span>Az</span><span>Çok</span></div></>}
      <ToggleRow title={tr.map.layer.coverage} sub={tr.map.layerHelp.coverage} on={L.coverage} onChange={(v) => set("coverage", v)} />
      {L.coverage && (
        <div className="legend">
          <span><i style={{ background: "#2E9E5B" }} />{tr.map.coverageLegend.strong}</span>
          <span><i style={{ background: "#E2A21C" }} />{tr.map.coverageLegend.medium}</span>
          <span><i style={{ background: "#D9472F" }} />{tr.map.coverageLegend.weak}</span>
          <span><i style={{ background: "#6B7280" }} />{tr.map.coverageLegend.none}</span>
        </div>
      )}
      <ToggleRow title={tr.map.layer.stations} on={L.stations} onChange={(v) => set("stations", v)} />
      <ToggleRow title={tr.map.layer.links} sub={tr.map.layerHelp.links} on={L.links} onChange={(v) => set("links", v)} />
      <ToggleRow title={tr.map.layer.me} on={L.me} onChange={(v) => set("me", v)} />

      <Section title={tr.map.range.title} />
      <Chips value={p.range} onChange={(r) => m.actions.setPrefs({ range: r })} options={RANGES.map((r) => ({ value: r, label: tr.map.range[r] }))} />
      <p className="fine" style={{ padding: "8px 20px 0" }}>{tr.map.tilesNote}</p>
    </Dialog>
  );
}
