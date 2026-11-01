// 4b: toplu ayar (sahip kararı: hepsi / grup / tek). Seçilen her cihazın
// istenen ayarı yazılır; her cihaz kendi ekranında "bekliyor" gösterir ve
// bağlanınca sormadan alır. Yalnız değiştirilen ayarlar yazılır.
import { useState } from "react";
import { SETTINGS, type SettingKey, type SettingValues } from "../../app/deviceSettings.ts";
import { useBack } from "../../app/nav.ts";
import { pilotWantSettings } from "../../app/ownerAccountPilotRuntime.ts";
import { useApp } from "../../app/store.ts";
import { Dialog, Seg } from "../../ds/components.tsx";

type Scope = "all" | "pick";

export function BulkSettingsDialog({ onClose }: { onClose: () => void }) {
  const m = useApp();
  const trackers = m.herd.filter((a) => a.device).map((a) => ({ id: a.device!, name: a.name }));
  const [scope, setScope] = useState<Scope>("all");
  const [groups, setGroups] = useState<string[]>([]);
  const [devices, setDevices] = useState<string[]>([]);
  const [values, setValues] = useState<SettingValues>({});
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState("");
  const [done, setDone] = useState("");
  useBack(onClose);

  const chosen = scope === "all" ? trackers.map((t) => t.id) : [...new Set([
    ...devices,
    ...m.targetBook.groups.filter((g) => groups.includes(g.id)).flatMap((g) => g.devices),
  ])].filter((d) => trackers.some((t) => t.id === d));
  const changed = Object.keys(values) as SettingKey[];

  const toggle = (list: string[], set: (v: string[]) => void, x: string, on: boolean) =>
    set(on ? [...list, x] : list.filter((y) => y !== x));

  async function save(): Promise<void> {
    setBusy(true); setError("");
    try {
      await pilotWantSettings(chosen, values);
      setDone(`${chosen.length} tasmanın ayarı kaydedildi. Her tasma bağlanınca alır; durumu tasmanın ekranında.`);
      setValues({});
    } catch (e) { setError(e instanceof Error ? e.message : "Kaydedilemedi."); }
    finally { setBusy(false); }
  }

  return (
    <Dialog title="Toplu ayar" onClose={onClose}>
      <div className="stack" style={{ padding: "0 20px 16px" }}>
        <span className="sm strong">Hangi tasmalar</span>
        <Seg<Scope> value={scope} onChange={setScope} options={[{ value: "all", label: "Hepsi" }, { value: "pick", label: "Seçerek" }]} />
        {scope === "pick" && (
          <div className="stack" style={{ gap: 6 }}>
            {m.targetBook.groups.map((g) => (
              <label key={g.id} className="sm"><input type="checkbox" checked={groups.includes(g.id)}
                onChange={(e) => toggle(groups, setGroups, g.id, e.target.checked)} /> {g.name} (grup · {g.devices.length})</label>
            ))}
            {trackers.map((t) => (
              <label key={t.id} className="sm"><input type="checkbox" checked={devices.includes(t.id)}
                onChange={(e) => toggle(devices, setDevices, t.id, e.target.checked)} /> {t.name}</label>
            ))}
          </div>
        )}
        <span className="sm">{chosen.length ? `${chosen.length} tasma` : "Henüz seçilmedi"}</span>

        <span className="sm strong">Ne değişsin</span>
        {SETTINGS.map((def) => {
          const v = values[def.key];
          return (
            <label key={def.key} className="kv" style={{ padding: 0 }}>
              <span>{def.title}</span>
              <select className="inp" style={{ width: "auto" }} aria-label={def.title}
                value={v === undefined ? "" : String(v)}
                onChange={(e) => {
                  const raw = e.target.value;
                  const next = { ...values };
                  if (raw === "") delete next[def.key];
                  else (next as Record<string, unknown>)[def.key] = def.kind === "flag" ? raw === "true" : Number(raw);
                  setValues(next);
                }}>
                <option value="">Değişmesin</option>
                {def.kind === "flag"
                  ? <><option value="true">Açık</option><option value="false">Kapalı</option></>
                  : def.choices!.map((c) => <option key={c} value={c}>{def.word(c)}</option>)}
              </select>
            </label>
          );
        })}
        <button className="btn primary wide" disabled={busy || !chosen.length || !changed.length} onClick={() => { void save(); }}>
          {chosen.length && changed.length ? `${chosen.length} tasmaya kaydet` : "Kaydet"}
        </button>
        {done && <p className="sm" role="status" style={{ margin: 0 }}>{done}</p>}
        {error && <p className="sm t-aging" role="alert" style={{ margin: 0 }}>{error}</p>}
      </div>
    </Dialog>
  );
}
