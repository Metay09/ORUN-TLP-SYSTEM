// Alanlar (haritadan açılır): the pasture boundaries. The list with each zone's size and who is
// inside, the collar's capacity (8 areas / 64 corners) and GeoJSON in and
// out. Drawing happens on the map. Sending to collars comes with step 3/4/6.
import { useRef, useState } from "react";
import { CaretRight, Plus, Polygon } from "@phosphor-icons/react";
import { fmt, tr } from "../../app/i18n/tr.ts";
import { useBack, useNav } from "../../app/nav.ts";
import { useApp } from "../../app/store.ts";
import { FENCE, areaM2, canonicalE7, compileFence, fromGeoJSON, newZoneId, toGeoJSON, validateRing, zoneAt } from "../../app/zones.ts";
import { Bar, Card, Cell, Empty, Header, Section, Toggle } from "../../ds/components.tsx";
import { targetsForZone } from "../../app/zoneTargets.ts";
import { ApplyFenceDialog, toApply, useQueuedFences, useTrackerFences } from "./ApplyFence.tsx";
import { useOps } from "../devices/Ops.tsx";
import { targetLabel } from "./ZoneTargets.tsx";

export function ZonesScreen() {
  const m = useApp();
  const nav = useNav();
  useBack(() => nav.pop(), nav.stack[nav.stack.length - 1]?.to === "zones");
  const file = useRef<HTMLInputElement>(null);
  const [note, setNote] = useState<string | null>(null);
  const [sending, setSending] = useState(false);
  const plans = useTrackerFences();
  const changed = toApply(plans, useQueuedFences());
  const queued = new Set(useOps().filter((r) => r.kind === "fence" && (r.state === "waiting" || r.state === "gateway")).map((r) => r.device)).size;
  const trackers = m.herd.filter((a) => a.device).map((a) => ({ id: a.device!, name: a.name }));
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
    <div className="screen" role="dialog" aria-label={tr.zones.title}>
      <Header title={tr.zones.title} onBack={() => nav.pop()}>
        <button className="btn small primary" onClick={() => nav.drawZone(null)}><Plus size={18} weight="bold" />{tr.zones.add}</button>
      </Header>
      <div className="page">
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

          <Section title="Tasmalara uygula" info="Alanı kaydetmek tasmadaki alanı değiştirmez. Alana dokun → 'Hangi tasmalar'ı seç, sonra Tasmalara uygula: her tasma için bir işlem açılır, tasmaya bağlanınca (şimdilik USB) işlenir; tasma onaylayınca 'Tasmaya işlendi' olur." />
          <Card pad>
            <div className="stack">
              <span className={"sm strong" + (changed.length ? " t-aging" : "")}>
                {changed.length ? `${changed.length} tasmanın alanları değişti`
                  : queued ? `${queued} tasmada alan işlemi sırada`
                  : plans.some((p) => p.state === "current") ? "Bütün tasmalarda alanlar güncel"
                  : "Alanlar için henüz tasma seçilmedi"}
              </span>
              {changed.length > 0 && <span className="fine">{changed.map((p) => p.name).join(", ")}</span>}
              {queued > 0 && <span className="fine">{changed.length ? `${queued} tasmada alan işlemi sırada · ` : ""}Tasmaya bağlanınca (USB ya da Bluetooth) işlenir; ilerleme Bağlantı → Bekleyen işlemler'de.</span>}
              {!plans.some((p) => p.state !== "none") && <span className="fine">Alana dokun → "Hangi tasmalar"dan Hepsi'ni ya da hayvanları seç.</span>}
              <button className="btn primary wide" disabled={!changed.length} onClick={() => setSending(true)}>Tasmalara uygula</button>
            </div>
          </Card>
        </>
      )}

      {sending && <ApplyFenceDialog onClose={() => setSending(false)} />}

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
    </div>
  );
}
