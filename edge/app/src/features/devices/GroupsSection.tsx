// Gruplar (2026-11-11'den beri Ayarlar'da, tasma ayarlarının altında): aynı gruplar alan göndermede
// ("Hangi tasmalar") ve toplu ayarda kullanılır.
import { useState } from "react";
import { Plus } from "@phosphor-icons/react";
import { useApp } from "../../app/store.ts";
import { Card, Dialog, Section } from "../../ds/components.tsx";

export function GroupsSection() {
  const m = useApp();
  const [open, setOpen] = useState(false);
  const [name, setName] = useState("");
  const [devices, setDevices] = useState<string[]>([]);
  const [note, setNote] = useState<string | null>(null);
  const trackers = m.herd.filter((a) => a.device).map((a) => ({ id: a.device!, name: a.name }));

  const save = () => {
    try {
      if (!name.trim() || !devices.length) throw new Error("Bir grup adı ve en az bir cihaz seç.");
      m.actions.saveTargetGroup({
        id: "g" + Date.now().toString(36) + Math.floor(Math.random() * 1e6).toString(36),
        name: name.trim().slice(0, 60), devices,
      });
      setOpen(false); setName(""); setDevices([]); setNote(null);
    } catch (e) { setNote(e instanceof Error ? e.message : "Grup kaydedilemedi."); }
  };

  return (
    <>
      <Section title="Gruplar" info="Gruplar, birkaç cihaza birden alan göndermek ve toplu ayar vermek için. Bir cihaz birden çok grupta olabilir." />
      <Card pad>
        <div className="stack">
          {m.targetBook.groups.map((g) => (
            <div className="row" key={g.id} style={{ justifyContent: "space-between" }}>
              <span className="sm">{g.name} · {g.devices.length} cihaz</span>
              <button className="btn small danger" onClick={() => {
                if (!window.confirm(g.name + " grubu silinsin mi?")) return;
                try { m.actions.removeTargetGroup(g.id); setNote(null); }
                catch (e) { setNote(e instanceof Error ? e.message : "Grup silinemedi."); }
              }}>Sil</button>
            </div>
          ))}
          <button className="btn small" disabled={!trackers.length || !!m.targetWarning} onClick={() => setOpen(true)}>
            <Plus size={18} /> Yeni grup
          </button>
          {note && <p className="sm t-aging" role="alert" style={{ margin: 0 }}>{note}</p>}
        </div>
      </Card>
      {open && (
        <Dialog title="Yeni grup" onClose={() => setOpen(false)}>
          <div className="stack" style={{ padding: "4px 16px 16px" }}>
            <input className="inp" value={name} maxLength={60}
              onChange={(e) => setName(e.target.value)} placeholder="Grup adı (ör. Düveler)" aria-label="Grup adı" />
            {trackers.map((t) => (
              <label key={t.id} className="sm">
                <input type="checkbox" checked={devices.includes(t.id)}
                  onChange={(e) => setDevices((v) => e.target.checked ? [...v, t.id] : v.filter((id) => id !== t.id))} />
                {" "}{t.name}
              </label>
            ))}
            <button className="btn primary wide" disabled={!name.trim() || !devices.length} onClick={save}>Kaydet</button>
          </div>
        </Dialog>
      )}
    </>
  );
}
