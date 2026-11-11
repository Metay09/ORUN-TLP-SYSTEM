// Ayarlar → Tasma ayarları (sahip kararı 2026-11-10): her ayar bütün sürü için
// TEK satır — ortak değer, kaç tasmada beklediği. Dokununca hepsine ya da
// seçilen tasmalara verilir. Tek tasmanın ekranı listenin altından açılır.
// Ayar hesaba kaydedilir; tasmaya bağlanınca kendiliğinden yazılır (4b, 4c-1b).
import { useState } from "react";
import { CaretRight } from "@phosphor-icons/react";
import {
  fleetCommon, fleetLine, fleetRows, pendingKeys, type FleetRow, type SettingValues,
} from "../../app/deviceSettings.ts";
import { useBack, useNav } from "../../app/nav.ts";
import { pilotAccountStatus, pilotWantSettings } from "../../app/ownerAccountPilotRuntime.ts";
import { useApp } from "../../app/store.ts";
import { Card, Cell, Dialog, Section, Seg, Toggle } from "../../ds/components.tsx";
import { useDeviceSettings } from "../devices/DeviceScreen.tsx";

interface Tracker { id: string; name: string; observed: SettingValues; }

export function TrackerSettingsPanel() {
  const m = useApp();
  const nav = useNav();
  const entries = useDeviceSettings();
  const [editing, setEditing] = useState<FleetRow | null>(null);
  const trackers: Tracker[] = m.herd.filter((a) => a.device).map((a) => ({
    id: a.device!.toUpperCase(), name: a.name, observed: a.periodS ? { interval_s: a.periodS } : {},
  }));
  const signedIn = Boolean(pilotAccountStatus());

  return (
    <>
      <Section title="Tasma ayarları"
        info="Ayar hesaba kaydedilir ve tasmaya iki yoldan gider: LoRa'dan, tasmanın bir sonraki raporunda gateway üzerinden (internetle sunucu; internetsiz gateway'e bağlı bu telefon); ya da tasmaya USB/Bluetooth ile bağlanınca. Hangisi önce ulaşırsa. Tasma onaylayana kadar 'bekliyor' görünür." />
      {!trackers.length ? (
        <Card pad><p className="sm" style={{ margin: 0 }}>Henüz tasma yok. Sürü'de + ile tasma ekleyince ayarları burada görünür.</p></Card>
      ) : (
        <>
          <Card>
            {fleetRows(trackers, entries).map((r) => {
              const line = fleetLine(r);
              return (
                <Cell key={r.def.key} title={r.def.title} wrapSub sub={line.sub} subTone={line.waiting ? "aging" : undefined}
                  trail={<span className="sm row" style={{ gap: 4 }}>{line.value}<CaretRight size={16} color="var(--mute)" /></span>}
                  onClick={() => setEditing(r)} />
              );
            })}
          </Card>
          {!signedIn && <p className="fine lead">Ayar değiştirmek için hesaba giriş yap.</p>}
          <details className="tech">
            <summary>Tek tasmanın ayarı · {trackers.length}</summary>
            <Card>
              {trackers.map((t) => {
                const n = pendingKeys(entries[t.id]).length;
                return (
                  <Cell key={t.id} title={t.name} sub={n ? `${n} ayar bekliyor` : "Bekleyen ayar yok"} subTone={n ? "aging" : undefined}
                    trail={<CaretRight size={16} color="var(--mute)" />} onClick={() => nav.push({ to: "device", id: t.id })} />
                );
              })}
            </Card>
          </details>
        </>
      )}
      {editing && <FleetEdit row={editing} trackers={trackers} onClose={() => setEditing(null)} />}
    </>
  );
}

function FleetEdit({ row, trackers, onClose }: { row: FleetRow; trackers: Tracker[]; onClose: () => void }) {
  const def = row.def;
  const [scope, setScope] = useState<"all" | "pick">("all");
  const [picked, setPicked] = useState<string[]>([]);
  const [value, setValue] = useState<number | boolean | null>(fleetCommon(row));
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState("");
  useBack(onClose);
  const chosen = scope === "all" ? trackers.map((t) => t.id) : picked;

  async function save(): Promise<void> {
    if (value === null || !chosen.length) return;
    setBusy(true); setError("");
    try { await pilotWantSettings(chosen, { [def.key]: value }); onClose(); }
    catch (e) { setError(e instanceof Error ? e.message : "Kaydedilemedi."); }
    finally { setBusy(false); }
  }

  return (
    <Dialog title={def.title} onClose={onClose}>
      <div className="stack" style={{ padding: "0 20px 16px" }}>
        <p className="sm" style={{ margin: 0 }}>{def.help}</p>
        {def.kind === "choice" ? (
          <select className="inp" aria-label={def.title} value={typeof value === "number" ? value : ""}
            onChange={(e) => setValue(e.target.value === "" ? null : Number(e.target.value))}>
            {value === null && <option value="">Seç</option>}
            {def.choices!.map((c) => <option key={c} value={c}>{def.word(c)}</option>)}
          </select>
        ) : (
          <div className="row" style={{ justifyContent: "space-between" }}>
            <span className="sm">{value === null ? "Seçilmedi" : def.word(value === true)}</span>
            <Toggle on={value === true} label={def.title} onChange={(v) => setValue(v)} />
          </div>
        )}
        <Seg<"all" | "pick"> value={scope} onChange={setScope}
          options={[{ value: "all", label: `Hepsi (${trackers.length})` }, { value: "pick", label: "Seçerek" }]} />
        {scope === "pick" && (
          <div className="stack" style={{ gap: 6 }}>
            {trackers.map((t) => (
              <label key={t.id} className="sm"><input type="checkbox" checked={picked.includes(t.id)}
                onChange={(e) => setPicked(e.target.checked ? [...picked, t.id] : picked.filter((x) => x !== t.id))} /> {t.name}</label>
            ))}
          </div>
        )}
        <div className="btns" style={{ padding: 0 }}>
          <button className="btn" onClick={onClose}>Vazgeç</button>
          <button className="btn primary" disabled={busy || value === null || !chosen.length} onClick={() => { void save(); }}>
            {chosen.length ? `${chosen.length} tasmaya kaydet` : "Kaydet"}
          </button>
        </div>
        {error && <p className="sm t-aging" role="alert" style={{ margin: 0 }}>{error}</p>}
      </div>
    </Dialog>
  );
}
