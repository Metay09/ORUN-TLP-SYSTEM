// Alanlar: the pasture boundaries. The list with each zone's size and who is
// inside, the collar's capacity (8 areas / 64 corners), serbest otlatma, and
// GeoJSON in and out. Drawing happens on the map.
import { useRef, useState } from "react";
import { CaretRight, Plus, Polygon } from "@phosphor-icons/react";
import { fmt, tr } from "../../app/i18n/tr.ts";
import { useNav } from "../../app/nav.ts";
import { useApp } from "../../app/store.ts";
import { FENCE, areaM2, canonicalE7, compileFence, fromGeoJSON, newZoneId, toGeoJSON, validateRing, zoneAt } from "../../app/zones.ts";
import { Bar, Card, Cell, DemoFlag, Empty, Header, Section, Toggle } from "../../ds/components.tsx";

export function ZonesScreen() {
  const m = useApp();
  const nav = useNav();
  const file = useRef<HTMLInputElement>(null);
  const [note, setNote] = useState<string | null>(null);
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

          <Section title={tr.zones.title} />
          <Card>
            {m.zones.map((z) => {
              const bad = validateRing(z.ring);
              const inside = insideOf(z.id);
              return (
                <Cell key={z.id}
                  lead={<span className={"ic " + (bad ? "alarm" : z.active ? "aging" : "")}><Polygon size={20} weight={z.active ? "fill" : "regular"} /></span>}
                  title={z.name}
                  sub={bad ? tr.zones.invalid : `${tr.zones.line(fmt.dec(areaM2(z.ring) / 10_000, 2), canonicalE7(z.ring).length, inside)}${z.active ? "" : ` · ${tr.zones.off}`}`}
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
          <p className="fine lead">{tr.zones.stored}</p>
        </>
      )}

      {/* "Serbest otlatma" is a tracker-owned setting, not an app-only
          mute switch. Do not show controls until a signed collar command and
          durable ACK exist; hiding them prevents a dangerous false promise. */}

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
