// 3a: bu alan hangi tasmalara gitsin: hepsi, gruplar ya da tek tek.
import { tr } from "../../app/i18n/tr.ts";
import { useApp } from "../../app/store.ts";
import { emptyRule, targetsForZone, type ZoneRule } from "../../app/zoneTargets.ts";
import { Card, Section, Seg } from "../../ds/components.tsx";

/** "Hepsi" / "3 tasma" / "Tasma seçilmedi" for the zone list. */
export function targetLabel(rule: ZoneRule | undefined, count: number): string {
  if (!rule) return "Tasma seçilmedi";
  if (rule.scope === "all") return "Bütün tasmalar";
  return count ? `${count} tasma` : "Tasma seçilmedi";
}

export function ZoneTargets({ zoneId, onError }: { zoneId: string; onError: (e: string | null) => void }) {
  const m = useApp();
  const rule = m.targetBook.rules[zoneId] ?? emptyRule();
  const trackers = m.herd.filter((a) => a.device).map((a) => ({ id: a.device!, name: a.name }));
  const targets = targetsForZone(m.targetBook, zoneId, trackers.map((t) => t.id));
  const save = (next: ZoneRule) => {
    try { m.actions.saveTargetRule(zoneId, next); onError(null); }
    catch (e) { onError(e instanceof Error ? e.message : "Seçim kaydedilemedi."); }
  };
  const has = Boolean(m.targetBook.rules[zoneId]);
  return (
    <>
      <Section title="Hangi tasmalar" info="Alan, burada seçilen tasmalara gönderilir: hepsine, seçtiğin gruplara ya da tek tek hayvanlara. Seçtikten sonra 'Tasmalara uygula'; LoRa'dan ya da tasmaya bağlanınca işlenir." />
      <Card pad>
        <div className="stack">
          {m.targetWarning && <span className="sm t-alarm strong">{m.targetWarning}</span>}
          <Seg<"all" | "selected" | "none"> value={has ? rule.scope : "none"}
            onChange={(v) => v === "all" ? save({ scope: "all", devices: [], groups: [] })
              : v === "selected" ? save({ ...emptyRule(), ...(has && rule.scope === "selected" ? rule : {}) })
              : undefined}
            options={[{ value: "all", label: "Hepsi" }, { value: "selected", label: "Seçerek" }]} />
          {has && rule.scope === "selected" && (
            <>
              {m.targetBook.groups.map((g) => (
                <label className="sm" key={g.id}>
                  <input type="checkbox" checked={rule.groups.includes(g.id)}
                    onChange={(e) => save({ ...rule,
                      groups: e.target.checked ? [...rule.groups, g.id] : rule.groups.filter((x) => x !== g.id) })} />
                  {" "}Grup: {g.name} ({g.devices.length} tasma)
                </label>
              ))}
              {trackers.map((t) => (
                <label className="sm" key={t.id}>
                  <input type="checkbox" checked={rule.devices.includes(t.id)}
                    onChange={(e) => save({ ...rule,
                      devices: e.target.checked ? [...rule.devices, t.id] : rule.devices.filter((x) => x !== t.id) })} />
                  {" "}{t.name}
                </label>
              ))}
              {!trackers.length && <span className="fine">Henüz tasma yok.</span>}
            </>
          )}
          <span className="sm strong">{has ? `Bu alan ${targets.length} tasmaya gidecek` : "Henüz seçilmedi; bu alan hiçbir tasmaya gitmez."}</span>
          {!has && <span className="fine">{tr.zones.title} listesinde "Tasma seçilmedi" yazanlar gönderilmez.</span>}
        </div>
      </Card>
    </>
  );
}
