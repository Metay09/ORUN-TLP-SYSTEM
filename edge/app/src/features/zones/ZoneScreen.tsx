// One zone: size, corners, who is inside now, whether the collar would accept
// it; show it on the map, edit the boundary, rename, switch off or delete.
import { useState } from "react";
import { MapTrifold, PencilSimple, Trash } from "@phosphor-icons/react";
import { fmt, tr } from "../../app/i18n/tr.ts";
import { useNav } from "../../app/nav.ts";
import { useApp } from "../../app/store.ts";
import { ERROR_TEXT, areaM2, canonicalE7, compileFence, perimeterM, validateRing, zoneAt } from "../../app/zones.ts";
import { emptyRule, targetsForZone, type ZoneRule } from "../../app/zoneTargets.ts";
import { Card, Dialog, KV, Screen, ToggleRow } from "../../ds/components.tsx";

export function ZoneScreen({ id }: { id: string }) {
  const m = useApp();
  const nav = useNav();
  const z = m.zones.find((x) => x.id === id);
  const [renaming, setRenaming] = useState(false);
  const [name, setName] = useState(z?.name ?? "");
  const [error, setError] = useState<string | null>(null);
  const rule = m.targetBook.rules[id] ?? emptyRule();
  const targets = targetsForZone(m.targetBook, id, m.devices.map((d) => d.id));
  const saveTargets = (next: ZoneRule) => {
    try { m.actions.saveTargetRule(id, next); setError(null); }
    catch (e) { setError(e instanceof Error ? e.message : "Hedef listesi kaydedilemedi."); }
  };
  const saveSafe = (next: typeof z) => {
    try { if (!next) return false; m.actions.saveZone(next); setError(null); return true; }
    catch (e) { setError(e instanceof Error ? e.message : "Mera kaydedilemedi."); return false; }
  };
  if (!z) return <Screen title={tr.zones.title} onBack={nav.pop}><p className="sm lead">{tr.zones.empty}</p></Screen>;

  const bad = validateRing(z.ring);
  const a = areaM2(z.ring);
  const f = compileFence([{ ...z, active: true }]);
  const inside = bad ? [] : m.herd.filter((x) => x.last && zoneAt(f, x.last) !== null);

  return (
    <Screen title={z.name} onBack={nav.pop}>
      {error && <p className="sm t-alarm strong lead" role="alert">{error}</p>}
      <Card>
        <KV k={tr.zones.area}>{fmt.dec(a / 10_000, 2)} ha · {fmt.dec(a / 1000, 1)} dekar</KV>
        <KV k={tr.zones.perimeter}>{fmt.distance(perimeterM(z.ring))}</KV>
        <KV k={tr.zones.vertices}>{canonicalE7(z.ring).length}</KV>
        <KV k={tr.zones.insideNow}>{inside.length}</KV>
        <div style={{ padding: "4px 16px 14px" }}>
          <span className={bad ? "sm t-alarm strong" : "fine"}>{bad ? tr.zones.refused(ERROR_TEXT[bad]) : tr.zones.accepted}</span>
          {inside.length > 0 && <p className="sm" style={{ margin: "6px 0 0" }}>{inside.map((x) => x.name).join(", ")}</p>}
        </div>
      </Card>

      <Card>
        <ToggleRow title={tr.zones.active} sub={tr.zones.activeHelp} on={z.active} onChange={(v) => { saveSafe({ ...z, active: v }); }} />
      </Card>

      <Section title="Hedef cihazlar (yerel taslak)" />
      <Card pad>
        <div className="stack">
          {m.targetWarning && <span className="sm t-alarm strong">{m.targetWarning}</span>}
          <span className="fine">Bu seçim yalnız telefonda saklanır; tasmaya gönderilmez ve sınırı etkinleştirmez.</span>
          <label className="sm">
            <input type="radio" name={"zone-scope-" + id} checked={rule.scope === "selected"}
              onChange={() => saveTargets({ ...rule, scope: "selected" })} /> Seçili hayvanlar ve gruplar
          </label>
          <label className="sm">
            <input type="radio" name={"zone-scope-" + id} checked={rule.scope === "all"}
              onChange={() => saveTargets({ scope: "all", devices: [], groups: [] })} /> Tüm bilinen tasmalar (taslak)
          </label>
          {rule.scope === "selected" && (
            <>
              {m.targetBook.groups.map((g) => (
                <label className="sm" key={g.id}>
                  <input type="checkbox" checked={rule.groups.includes(g.id)}
                    onChange={(e) => saveTargets({ ...rule,
                      groups: e.target.checked ? [...rule.groups, g.id] : rule.groups.filter((x) => x !== g.id) })} />
                  Grup: {g.name} ({g.devices.length} tasma)
                </label>
              ))}
              {m.devices.map((d) => (
                <label className="sm" key={d.id}>
                  <input type="checkbox" checked={rule.devices.includes(d.id)}
                    onChange={(e) => saveTargets({ ...rule,
                      devices: e.target.checked ? [...rule.devices, d.id] : rule.devices.filter((x) => x !== d.id) })} />
                  {m.herd.find((a) => a.device === d.id)?.name ?? d.short} · {d.short}
                </label>
              ))}
            </>
          )}
          <span className="sm strong">Planlanan hedef: {targets.length} tasma</span>
          <span className="fine">Hedef seçmek teslim veya uygulanma onayı değildir. Gerçek yükleme ayrıca doğrulanmalıdır.</span>
        </div>
      </Card>

      <div className="stack" style={{ padding: "16px 16px 0" }}>
        <button className="btn primary wide" onClick={() => nav.showZone(z.id)}><MapTrifold size={20} />{tr.zones.showOnMap}</button>
        <button className="btn wide" onClick={() => nav.drawZone(z.id)}><PencilSimple size={20} />{tr.zones.edit}</button>
        <button className="btn wide" onClick={() => { setName(z.name); setRenaming(true); }}>{tr.zones.rename}</button>
        <button className="btn danger wide" onClick={() => { if (window.confirm(tr.zones.removeConfirm(z.name))) {
          try { m.actions.removeZone(z.id); nav.pop(); }
          catch (e) { setError(e instanceof Error ? e.message : "Mera silinemedi."); }
        } }}>
          <Trash size={20} />{tr.zones.remove}
        </button>
      </div>

      {renaming && (
        <Dialog title={tr.zones.rename} onClose={() => setRenaming(false)}>
          <div className="stack" style={{ padding: "4px 16px 16px" }}>
            <input className="inp" value={name} maxLength={40} autoFocus aria-label={tr.zones.name} onChange={(e) => setName(e.target.value)}
              onKeyDown={(e) => { if (e.key === "Enter" && name.trim() && saveSafe({ ...z, name })) setRenaming(false); }} />
            <button className="btn primary wide" disabled={!name.trim()} onClick={() => { if (saveSafe({ ...z, name })) setRenaming(false); }}>{tr.zones.draw.save}</button>
          </div>
        </Dialog>
      )}
    </Screen>
  );
}
