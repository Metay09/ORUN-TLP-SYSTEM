// 3a: alanları USB ile bağlı tasmaya gönder. Tasmalar ve durumları listelenir;
// telefona hangi tasma bağlanırsa ona, o tasmaya ait tam alan listesi yazılır.
import { useState, useSyncExternalStore } from "react";
import { CheckCircle, Usb, WarningCircle } from "@phosphor-icons/react";
import { probeUsbDevice } from "../../app/addTracker.ts";
import { writeFence } from "../../app/fenceUsb.ts";
import { trackerFencePlans, type TrackerFence } from "../../app/fencePlan.ts";
import { fmt } from "../../app/i18n/tr.ts";
import { canonicalE7 } from "../../app/zones.ts";
import { useBack } from "../../app/nav.ts";
import { pilotRecordFence, trackerFences } from "../../app/ownerAccountPilotRuntime.ts";
import { useApp } from "../../app/store.ts";
import { Bar, Cell, Dialog } from "../../ds/components.tsx";
import { UsbConsole } from "../../platform/usbConsole.ts";

export function stateLine(p: TrackerFence): string {
  if (p.state === "none") return "Bu tasmaya gidecek alan yok";
  const plan = `${p.zones.length} alan · ${p.zones.reduce((n, z) => n + canonicalE7(z.ring).length, 0)} köşe`;
  if (p.state === "current") return `Tasmada güncel · ${fmt.dateTime(p.record!.appliedAt)}`;
  if (p.state === "stale") return `Tasmadaki alanlar eski · gidecek: ${plan}`;
  return `Hiç gönderilmedi · gidecek: ${plan}`;
}

export function useTrackerFences(): TrackerFence[] {
  const m = useApp();
  const records = useSyncExternalStore((fn) => trackerFences.subscribe(fn), () => trackerFences.snapshot());
  const trackers = m.herd.filter((a) => a.device).map((a) => ({ device: a.device!, name: a.name }));
  return trackerFencePlans(m.targetBook, m.zones, trackers, records);
}

export function SendFenceDialog({ onClose }: { onClose: () => void }) {
  const m = useApp();
  const plans = useTrackerFences();
  const [busy, setBusy] = useState(false);
  const [progress, setProgress] = useState(0);
  const [status, setStatus] = useState("");
  const [error, setError] = useState("");
  useBack(() => { if (!busy) onClose(); });

  async function send(): Promise<void> {
    setBusy(true); setError(""); setStatus("USB açılıyor…"); setProgress(0);
    let con: UsbConsole | null = null;
    try {
      con = await UsbConsole.open();
      setStatus("Tasma okunuyor…");
      const dev = await probeUsbDevice(con);
      if (dev.kind !== "tracker") throw new Error("Bu bir gateway; alanlar tasmaya yazılır.");
      const id = dev.report.device.toUpperCase();
      const p = plans.find((x) => x.device.toUpperCase() === id);
      if (!p) throw new Error("Bu tasma Sürü'de görünmüyor; önce Sürü → \"+\" ile ekle ve konumu gelsin.");
      if (p.errors.length) throw new Error(p.errors.join(" "));
      if (!p.zones.length) throw new Error(`${p.name}: bu tasmaya gidecek alan yok. Alanın "Hangi tasmalar" ayarına bak.`);
      setStatus(`${p.name}: ${p.zones.length} alan yazılıyor…`);
      const sum = await writeFence(con, p.zones, (d, t) => setProgress(d / t));
      await pilotRecordFence(id, {
        digest: sum.digest, areas: sum.areas, vertices: sum.vertices,
        zones: p.zones.map((z) => ({ id: z.id, name: z.name })), appliedAt: Date.now(),
      });
      setStatus(`${p.name}: ${sum.areas} alan, ${sum.vertices} köşe tasmaya yazıldı ve tasma onayladı. Sıradaki tasmayı bağlayabilirsin.`);
    } catch (e) {
      setStatus("");
      setError(e instanceof Error && e.message.length < 300 ? e.message : "Alanlar gönderilemedi.");
    } finally {
      if (con) await con.close();
      setBusy(false);
    }
  }

  const gatewayOnUsb = m.station.link === "usb";
  const order = { stale: 0, never: 1, current: 2, none: 3 } as const;
  const sorted = [...plans].sort((x, y) => order[x.state] - order[y.state]);
  return (
    <Dialog title="Alanları tasmaya gönder" onClose={() => { if (!busy) onClose(); }}>
      <div className="stack" style={{ padding: "0 20px 16px" }}>
        <p className="sm" style={{ margin: 0 }}>Tasmayı USB (OTG) kablosuyla telefona bağla ve Gönder'e bas. O tasmaya ait bütün alanlar yazılır; tasmadaki eski liste yerine geçer.</p>
        <button className="btn primary wide" disabled={busy || gatewayOnUsb} onClick={() => { void send(); }}>
          <Usb size={20} />{busy ? "Gönderiliyor…" : "USB ile bağlı tasmaya gönder"}
        </button>
        {gatewayOnUsb && <p className="fine">Önce gateway'in USB bağlantısını kes (Bağlantı sekmesi).</p>}
        {busy && progress > 0 && <Bar ratio={progress} />}
        {status && <p className="sm" role="status">{status}</p>}
        {error && <p role="alert" className="sm t-aging">{error}</p>}
        <div className="card" style={{ margin: 0 }}>
          {plans.length ? sorted.map((p) => (
            <Cell key={p.device} title={p.name} wrapSub
              lead={<span className={"ic " + (p.state === "current" ? "fresh" : p.state === "none" ? "" : "aging")}>
                {p.state === "current" ? <CheckCircle size={20} /> : <WarningCircle size={20} />}</span>}
              sub={stateLine(p)} />
          )) : <p className="sm" style={{ padding: "12px 16px", margin: 0 }}>Henüz tasma yok.</p>}
        </div>
      </div>
    </Dialog>
  );
}
