// Alanlar: the pasture boundaries. The list with each zone's size and who is
// inside, the collar's capacity (8 areas / 64 corners) and GeoJSON in and
// out. Drawing happens on the map. Sending to collars comes with step 3/4/6.
import { useRef, useState } from "react";
import { CaretRight, Plus, Polygon, Usb } from "@phosphor-icons/react";
import { fmt, tr } from "../../app/i18n/tr.ts";
import { useNav } from "../../app/nav.ts";
import { useApp } from "../../app/store.ts";
import { FENCE, areaM2, canonicalE7, compileFence, fromGeoJSON, newZoneId, toGeoJSON, validateRing, zoneAt } from "../../app/zones.ts";
import { Bar, Card, Cell, DemoFlag, Dialog, Empty, Header, Section, Toggle } from "../../ds/components.tsx";
import { needsSending } from "../../app/fencePlan.ts";
import { targetsForZone } from "../../app/zoneTargets.ts";
import { SendFenceDialog, useTrackerFences } from "./SendFenceDialog.tsx";
import { targetLabel } from "./ZoneTargets.tsx";

export function ZonesScreen() {
  const m = useApp();
  const nav = useNav();
  const file = useRef<HTMLInputElement>(null);
  const [note, setNote] = useState<string | null>(null);
  const [sending, setSending] = useState(false);
  const [groupOpen, setGroupOpen] = useState(false);
  const [groupName, setGroupName] = useState("");
  const [groupDevices, setGroupDevices] = useState<string[]>([]);
  const plans = useTrackerFences();
  const waiting = needsSending(plans);
  const trackers = m.herd.filter((a) => a.device).map((a) => ({ id: a.device!, name: a.name }));
  const saveGroup = () => {
    try {
      if (!groupName.trim() || !groupDevices.length) throw new Error("Bir grup adı ve en az bir hayvan seç.");
      m.actions.saveTargetGroup({
        id: "g" + Date.now().toString(36) + Math.floor(Math.random() * 1e6).toString(36),
        name: groupName.trim().slice(0, 60), devices: groupDevices,
      });
      setGroupOpen(false); setGroupName(""); setGroupDevices([]); setNote(null);
    } catch (e) { setNote(e instanceof Error ? e.message : "Grup kaydedilemedi."); }
  };
  const set = m.zoneSet;
  const located = m.herd.filter((a) => a.last);

  const insideOf = (id: string) => {
    if (!located.length) return null;
    const z = m.zones.find((x) => x.id === id);
    if (!z || !z.active || validateRing(z.ring)) return null;
    const f = compileFence([z]);
    return located.filter((a) => zoneAt(f, a.last!) !== null).length;
  };

  const importFile = async (f: File) => {
    try {
      const r = fromGeoJSON(await f.text(), Date.now(), newZoneId);
      if (!r.zones.length) throw new Error(r.skipped.join(" · ") || "alan bulunamadı");
      m.actions.addZones(r.zones);
      setNote([tr.zones.imported(r.zones.length), r.skipped.length ? tr.zones.skipped(r.skipped.join(" · ")) : ""].filter(Boolean).join(" · "));
    } catch (e) {
      setNote(tr.zones.importFailed(e instanceof Error ? e.message : String(e)));
    }
  };

  const exportGeo = async () => {
    const text = toGeoJSON(m.zones);
    try { await navigator.clipboard.writeText(text); setNote(tr.zones.copied); } catch { /* fall back to a file below */ }
    try {
      const url = URL.createObjectURL(new Blob([text], { type: "application/geo+json" }));
      const a = document.createElement("a");
      a.href = url; a.download = "orun-alanlar.geojson"; a.click();
      setTimeout(() => URL.revokeObjectURL(url), 2000);
    } catch { /* clipboard was enough */ }
  };

  return (
    <div className="page">
      <Header title={tr.zones.title}>
        <button className="btn small primary" onClick={() => nav.drawZone(null)}><Plus size={18} weight="bold" />{tr.zones.add}</button>
      </Header>
      {m.demo && <DemoFlag onPage />}
      {m.zoneMigrationWarning && <p className="sm t-alarm strong lead" role="alert">{m.zoneMigrationWarning}</p>}
      {note && <p className="sm t-alarm lead" role="status">{note}</p>}

      {!m.zones.length ? (
        <Empty icon={<Polygon size={34} />} title={tr.zones.empty} body={tr.zones.emptyBody}>
          <button className="btn primary" onClick={() => nav.drawZone(null)}><Plus size={18} weight="bold" />{tr.zones.add}</button>
          <span className="fine">{tr.zones.rules}</span>
        </Empty>
      ) : (
        <>
          <Section title={tr.zones.capacity} />
          <Card pad>
            <div className="stack">
              <div className="row" style={{ justifyContent: "space-between" }}>
                <span className="sm strong">{tr.zones.capacityLine(set.active.length, FENCE.maxAreas, set.total, FENCE.maxTotalVertices)}</span>
              </div>
              <Bar ratio={set.total / FENCE.maxTotalVertices} tone={set.ok ? (set.total / FENCE.maxTotalVertices >= 0.85 ? "warn" : undefined) : "bad"} />
              {set.ok ? <span className="fine">{tr.zones.setOk}</span> : set.errors.map((e) => <span key={e} className="sm t-alarm strong">{e}</span>)}
            </div>
          </Card>

          <Section title={tr.zones.title} info={tr.zones.stored} />
          <Card>
            {m.zones.map((z) => {
              const bad = validateRing(z.ring);
              const inside = insideOf(z.id);
              return (
                <Cell key={z.id}
                  lead={<span className={"ic " + (bad ? "alarm" : z.active ? "aging" : "")}><Polygon size={20} weight={z.active ? "fill" : "regular"} /></span>}
                  title={z.name}
                  sub={bad ? tr.zones.invalid : `${tr.zones.line(fmt.dec(areaM2(z.ring) / 10_000, 2), canonicalE7(z.ring).length, inside)}${z.active ? "" : ` · ${tr.zones.off}`} · ${targetLabel(m.targetBook.rules[z.id], targetsForZone(m.targetBook, z.id, trackers.map((t) => t.id)).length)}`}
                  subTone={bad ? "alarm" : undefined} wrapSub
                  trail={<span className="row" style={{ gap: 6 }}>
                    <span onClick={(e) => e.stopPropagation()}><Toggle on={z.active} label={`${z.name}: ${tr.zones.active}`} onChange={(v) => {
                      try { m.actions.saveZone({ ...z, active: v }); setNote(null); }
                      catch (e) { setNote(e instanceof Error ? e.message : "Alan etkinleştirilemedi."); }
                    }} /></span>
                    <CaretRight size={18} color="var(--mute)" />
                  </span>}
                  onClick={() => nav.push({ to: "zone", id: z.id })} />
              );
            })}
          </Card>

          <Section title="Tasmaya gönder" info="Alanlar tasmaya USB kablosuyla yazılır; tasma onaylayınca hesabına kaydedilir. Her tasmaya yalnız o tasma için seçilen açık alanlar gider. Bluetooth ve uzaktan (LoRa) gönderme sonraki adımlarda." />
          <Card pad>
            <div className="stack">
              <span className={"sm strong" + (waiting.length ? " t-aging" : "")}>
                {waiting.length ? `${waiting.length} tasmada alanlar eksik ya da eski`
                  : plans.some((p) => p.state === "current") ? "Bütün tasmalarda alanlar güncel"
                  : "Alanlar için henüz tasma seçilmedi"}
              </span>
              {waiting.length > 0 && <span className="fine">{waiting.map((p) => p.name).join(", ")}</span>}
              {!plans.some((p) => p.state !== "none") && <span className="fine">Alana dokun → "Hangi tasmalar"dan Hepsi'ni ya da hayvanları seç.</span>}
              <button className="btn primary wide" disabled={!plans.some((p) => p.state !== "none")} onClick={() => setSending(true)}><Usb size={20} />Tasmaya gönder</button>
            </div>
          </Card>
        </>
      )}

      <Section title="Gruplar" info="Gruplar, bir alanı birkaç hayvana birden göndermek için. Alanın 'Hangi tasmalar' ayarında grubu seçersin." />
      <Card pad>
        <div className="stack">
          {m.targetBook.groups.map((g) => (
            <div className="row" key={g.id} style={{ justifyContent: "space-between" }}>
              <span className="sm">{g.name} · {g.devices.length} tasma</span>
              <button className="btn small danger" onClick={() => {
                if (!window.confirm(g.name + " grubu silinsin mi?")) return;
                try { m.actions.removeTargetGroup(g.id); setNote(null); }
                catch (e) { setNote(e instanceof Error ? e.message : "Grup silinemedi."); }
              }}>Sil</button>
            </div>
          ))}
          <button className="btn small" disabled={!trackers.length || !!m.targetWarning} onClick={() => setGroupOpen(true)}>
            <Plus size={18} /> Yeni grup
          </button>
        </div>
      </Card>
      {groupOpen && (
        <Dialog title="Yeni grup" onClose={() => setGroupOpen(false)}>
          <div className="stack" style={{ padding: "4px 16px 16px" }}>
            <input className="inp" value={groupName} maxLength={60}
              onChange={(e) => setGroupName(e.target.value)} placeholder="Grup adı (ör. Düveler)" aria-label="Grup adı" />
            {trackers.map((t) => (
              <label key={t.id} className="sm">
                <input type="checkbox" checked={groupDevices.includes(t.id)}
                  onChange={(e) => setGroupDevices((v) => e.target.checked ? [...v, t.id] : v.filter((id) => id !== t.id))} />
                {" "}{t.name}
              </label>
            ))}
            <button className="btn primary wide" disabled={!groupName.trim() || !groupDevices.length} onClick={saveGroup}>Kaydet</button>
          </div>
        </Dialog>
      )}
      {sending && <SendFenceDialog onClose={() => setSending(false)} />}

      <Section title={tr.zones.file} />
      <Card pad>
        <div className="stack">
          <button className="btn wide" onClick={() => file.current?.click()}>{tr.zones.importGeo}</button>
          {m.zones.length > 0 && <button className="btn wide" onClick={() => void exportGeo()}>{tr.zones.exportGeo}</button>}
          <input ref={file} type="file" accept=".geojson,.json,application/geo+json,application/json" hidden
            onChange={(e) => { const f = e.target.files?.[0]; e.target.value = ""; if (f) void importFile(f); }} />
        </div>
      </Card>
    </div>
  );
}
