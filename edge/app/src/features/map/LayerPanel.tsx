// Map type, 3D and layer switches, the period, and the legends.
import { tr } from "../../app/i18n/tr.ts";
import { useApp } from "../../app/store.ts";
import type { BaseMap, Layers, Range } from "../../app/stores.ts";
import { Chips, Dialog, Section, ToggleRow } from "../../ds/components.tsx";

const BASES: BaseMap[] = ["satellite", "terrain", "street"];
const RANGES: Range[] = ["today", "h24", "d3", "d7", "d30"];

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
      {p.base !== "street" && <ToggleRow title={tr.map.layer.relief} on={L.relief} onChange={(v) => set("relief", v)} />}

      <Section title={tr.map.layer.title} />
      <ToggleRow title={tr.map.layer.animals} on={L.animals} onChange={(v) => set("animals", v)} />
      <ToggleRow title={tr.map.layer.trail} sub={tr.map.layerHelp.trail} on={L.trail} onChange={(v) => set("trail", v)} />
      <ToggleRow title={tr.map.layer.heat} sub={tr.map.layerHelp.heat} on={L.heat} onChange={(v) => set("heat", v)} />
      {L.heat && <><div className="heat-ramp" /><div className="legend" style={{ justifyContent: "space-between", paddingTop: 4 }}><span>Az</span><span>Çok</span></div></>}
      <ToggleRow title={tr.map.layer.zones} sub="Bu telefondaki mera taslakları; tasmaya uygulanmış sınırlar değil" on={L.zones} onChange={(v) => set("zones", v)} />
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
      <ToggleRow title={tr.map.layer.me} on={L.me} onChange={(v) => set("me", v)} />

      <Section title={tr.map.range.title} />
      <Chips value={p.range} onChange={(r) => m.actions.setPrefs({ range: r })} options={RANGES.map((r) => ({ value: r, label: tr.map.range[r] }))} />
      <p className="fine" style={{ padding: "8px 20px 0" }}>{tr.map.tilesNote}</p>
    </Dialog>
  );
}
