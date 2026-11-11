// Tek alan akışı (sahip kararı 2026-11-02, ORUN · Saha'dan): çiz → kaydet →
// hedef tasmalar ("Hangi tasmalar") → Tasmalara uygula. Uygulamak bir işlem
// açar; her tasma "Sırada" olur, tasmaya bağlanınca (şimdilik USB) işlenir ve
// ancak tasma onaylayınca "Tasmaya işlendi" olur. Taslağı kaydetmek cihazdaki
// alanı değiştirmez.
import { useState, useSyncExternalStore } from "react";
import { Usb } from "@phosphor-icons/react";
import { opDigest, toApply, trackerFencePlans, type TrackerFence } from "../../app/fencePlan.ts";

export { toApply };
import { fmt } from "../../app/i18n/tr.ts";
import { useBack } from "../../app/nav.ts";
import { pilotCreateFenceOp, trackerFences } from "../../app/ownerAccountPilotRuntime.ts";
import { useApp } from "../../app/store.ts";
import { canonicalE7 } from "../../app/zones.ts";
import { Cell, Dialog } from "../../ds/components.tsx";
import { TrackerLiveDialog } from "../herd/TrackerLive.tsx";
import { OpSteps, opSub, useOps } from "../devices/Ops.tsx";

export function useTrackerFences(): TrackerFence[] {
  const m = useApp();
  const records = useSyncExternalStore((fn) => trackerFences.subscribe(fn), () => trackerFences.snapshot());
  const trackers = m.herd.filter((a) => a.device).map((a) => ({ device: a.device!, name: a.name }));
  return trackerFencePlans(m.targetBook, m.zones, trackers, records);
}

const corners = (p: TrackerFence) => p.zones.reduce((n, z) => n + canonicalE7(z.ring).length, 0);

export function stateLine(p: TrackerFence): string {
  if (p.state === "none") return "Bu tasmaya gidecek alan yok";
  if (p.state === "clear") return `Tasmada ${p.record!.areas} alan var, hesapta yok · tasmadan silinecek (USB ile)`;
  const plan = `${p.zones.length} alan · ${corners(p)} köşe`;
  if (p.state === "current") return `Tasmada güncel · ${fmt.dateTime(p.record!.appliedAt)}`;
  if (p.state === "stale") return `Tasmadaki alanlar eski · yeni: ${plan}`;
  return `Tasmada alan yok · yeni: ${plan}`;
}

/** device → digest of its open zone operation. */
export function useQueuedFences(): Map<string, string> {
  const ops = useOps();
  const out = new Map<string, string>();
  for (const r of ops) if (r.kind === "fence" && (r.state === "waiting" || r.state === "gateway") && !out.has(r.device))
    out.set(r.device, (r.payload as { digest: string }).digest);
  return out;
}

export function ApplyFenceDialog({ onClose, only }: { onClose: () => void; only?: string }) {
  const plans = useTrackerFences();
  const ops = useOps();
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState("");
  const [op, setOp] = useState<string | null>(null);
  const [usb, setUsb] = useState(false);
  const [ble, setBle] = useState<string | null>(null);
  const [off, setOff] = useState<Set<string>>(new Set());   // seçilmeyenler (varsayılan: hepsi seçili)
  useBack(() => { if (!busy) onClose(); });

  const queued = useQueuedFences();
  const changed = toApply(plans, queued, only);
  const broken = plans.filter((p) => p.errors.length && (!only || p.device.toUpperCase() === only.toUpperCase()));
  const mine = op ? ops.filter((r) => r.op === op) : [];
  const picked = changed.filter((p) => !off.has(p.device.toUpperCase()));
  const toggle = (device: string, on: boolean) => setOff((prev) => {
    const next = new Set(prev); const d = device.toUpperCase();
    if (on) next.delete(d); else next.add(d);
    return next;
  });

  async function apply(): Promise<void> {
    setBusy(true); setError("");
    try {
      const id = await pilotCreateFenceOp(picked.map((p) => ({
        device: p.device,
        payload: { digest: opDigest(p), areas: p.zones.length, vertices: corners(p),
                   zones: p.zones.map((z) => ({ id: z.id, name: z.name, ring: z.ring })) },
      })));
      setOp(id);
    } catch (e) { setError(e instanceof Error ? e.message : "Uygulanamadı."); }
    finally { setBusy(false); }
  }

  const name = (device: string) => plans.find((p) => p.device.toUpperCase() === device)?.name ?? device;

  return (
    <Dialog title="Tasmalara uygula" onClose={() => { if (!busy) onClose(); }}>
      <div className="stack" style={{ padding: "0 20px 16px" }}>
        {!op ? <>
          <p className="sm" style={{ margin: 0 }}>
            {changed.length
              ? `${changed.length} tasmanın alanları değişti. Uygulanacakları seç; her tasma için bir işlem açılır, tasmaya bağlanınca (şimdilik USB) kendiliğinden işlenir.`
              : "Bütün tasmalardaki alanlar güncel; uygulanacak bir şey yok."}
          </p>
          <button className="btn primary wide" disabled={busy || !picked.length} onClick={() => { void apply(); }}>
            {picked.length ? `Uygula (${picked.length} tasma)` : changed.length ? "Tasma seçilmedi" : "Uygula"}
          </button>
          {changed.length > 1 && (
            <div className="row" style={{ gap: 8 }}>
              <button className="btn small" onClick={() => setOff(new Set())}>Hepsini seç</button>
              <button className="btn small" onClick={() => setOff(new Set(changed.map((p) => p.device.toUpperCase())))}>Hiçbiri</button>
            </div>
          )}
          {changed.length > 0 && (
            <div className="card" style={{ margin: 0 }}>
              {changed.map((p) => {
                const open = queued.has(p.device.toUpperCase());
                const on = !off.has(p.device.toUpperCase());
                return <Cell key={p.device} title={p.name} wrapSub
                  lead={<input type="checkbox" checked={on} aria-label={p.name} onChange={(e) => toggle(p.device, e.target.checked)}
                    onClick={(e) => e.stopPropagation()} style={{ width: 20, height: 20 }} />}
                  onClick={() => toggle(p.device, !on)}
                  sub={`${p.state === "clear" ? `Alanlar silinecek (tasmada ${p.record!.areas} alan var) · yalnız USB ile` : `${p.zones.map((z) => z.name).join(", ")} · ${p.zones.length} alan · ${corners(p)} köşe`}${open ? ` · önceki işlem sırada (yerine geçer)` : ""}`} />;
              })}
            </div>
          )}
          {broken.map((p) => <p key={p.device} className="sm t-aging" style={{ margin: 0 }}>{p.name}: {p.errors.join(" ")}</p>)}
        </> : <>
          <p className="sm" style={{ margin: 0 }}>İşlem açıldı. Tasma onaylayınca "Tasmaya işlendi" olur.</p>
          <button className="btn primary wide" onClick={() => setUsb(true)}><Usb size={20} />USB ile bağlı tasmaya işle</button>
          <div className="card" style={{ margin: 0 }}>
            {mine.map((t) => <Cell key={t.device} title={name(t.device)} wrapSub sub={<>{opSub(t)}<OpSteps t={t} /></>}
              onClick={() => setBle(t.device)}
              subTone={t.state === "applied" || t.state === "same" ? "fresh" : t.state === "rejected" ? "alarm" : undefined} />)}
          </div>
          <p className="fine" style={{ margin: 0 }}>Tasmayı OTG kablosuyla bağla ya da listeden bir tasmaya dokunup Bluetooth ile bağlan (tasmanın Bluetooth'u açılıştan sonra 10 dk, sonra her rapordan sonra kısa süre açık); sıradaki alanlar ve ayarlar sormadan yazılır.</p>
        </>}
        {error && <p role="alert" className="sm t-aging" style={{ margin: 0 }}>{error}</p>}
      </div>
      {usb && <TrackerLiveDialog device={null} target={{ usb: true }} onClose={() => setUsb(false)} />}
      {ble && <TrackerLiveDialog device={ble} target={{ find: ble }} onClose={() => setBle(null)} />}
    </Dialog>
  );
}
