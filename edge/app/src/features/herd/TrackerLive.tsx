// 4a: tasmaya dokununca canlı bağlanır, durumunu okur. Bluetooth ve USB aynı
// kartı gösterir (aynı soruların aynı yanıtları).
import { useEffect, useRef, useState } from "react";
import { ArrowClockwise, Bluetooth, Usb } from "@phosphor-icons/react";
import { noRoleMessage, probeUsbDevice } from "../../app/addTracker.ts";
import { bleName } from "../../app/deviceNames.ts";
import { fmt } from "../../app/i18n/tr.ts";
import { useBack, useNav } from "../../app/nav.ts";
import { useApp } from "../../app/store.ts";
import {
  batteryWord, buildStatus, gnssWord, readRawUsb, svcWord, type TrackerStatus,
} from "../../app/trackerStatus.ts";
import { Dialog, KV } from "../../ds/components.tsx";
import { pendingKeys, valuesFromStatus } from "../../app/deviceSettings.ts";
import { deliverUsb } from "../../app/deliver.ts";
import { deviceOps, deviceSettings, pilotOpResult, pilotRecordFence, pilotReportSettings } from "../../app/ownerAccountPilotRuntime.ts";
import { TrackerBle } from "../../platform/trackerBle.ts";
import { UsbConsole } from "../../platform/usbConsole.ts";
import { useTrackerFences } from "../zones/ApplyFence.tsx";

/** ble: a device from the scan list; find: look for this ID's Bluetooth name; usb: the cable. */
export type LiveTarget = { ble: string } | { find: string } | { usb: true };

const BT_OFF = "Tasmanın Bluetooth'u kapalı olabilir. Tasma açıldıktan sonra 10 dakika açık kalır, sonra pil için kapanır: tasmayı kapatıp aç ve tekrar dene. Ya da USB ile bağlan.";

function errText(e: unknown): string {
  return e instanceof Error && e.message.length < 240 ? e.message : "Cihaza bağlanılamadı.";
}

export function TrackerLiveDialog({ device, target, onClose, onAdd, onBridge, fromAnimal = false }: {
  device: string | null; target: LiveTarget; onClose: () => void;
  onAdd?: () => void; onBridge?: () => void; fromAnimal?: boolean;
}) {
  const m = useApp();
  const nav = useNav();
  const fences = useTrackerFences();
  const [id, setId] = useState<string | null>(device);
  const [status, setStatus] = useState<TrackerStatus | null>(null);
  const [phase, setPhase] = useState("Bağlanıyor…");
  const [error, setError] = useState("");
  const [synced, setSynced] = useState("");
  const link = useRef<TrackerBle | null>(null);
  const alive = useRef(true);
  useBack(onClose);

  async function readBle(): Promise<void> {
    if (!link.current) {
      let bleId = "ble" in target ? target.ble : null;
      if ("find" in target) {
        setPhase("Tasma aranıyor (Bluetooth)…");
        bleId = await TrackerBle.find(target.find);
        if (!bleId) throw new Error(BT_OFF);
      }
      setPhase("Bluetooth ile bağlanıyor…");
      link.current = await TrackerBle.open(bleId!);
    }
    setPhase("Okunuyor…");
    const raw = await link.current.readRaw();
    const st = buildStatus(raw, "ble", Date.now());
    if (alive.current) setStatus(st);
    if (id && st.role === "tracker") void pilotReportSettings(id, valuesFromStatus(st), "ble").catch(() => undefined);
  }

  async function readUsb(): Promise<void> {
    let con: UsbConsole | null = null;
    try {
      setPhase("USB açılıyor…");
      con = await UsbConsole.open();
      const dev = await probeUsbDevice(con);
      if (dev.kind === "gateway") throw new Error("USB'deki cihaz bir gateway. Gateway'e Cihazlar → USB'den bağlan.");
      if (dev.kind === "unassigned") throw new Error(noRoleMessage("UNASSIGNED") + " Cihazlar → \"USB ile bağlı cihaza bağlan\"dan kur.");
      if (dev.kind === "conflict") throw new Error(noRoleMessage("CONFLICT"));
      if (alive.current) setId(dev.report.device);
      setPhase("Okunuyor…");
      const dev_ = dev.report.device.toUpperCase();
      let st = buildStatus(await readRawUsb(con), "usb", Date.now());
      if (alive.current) setStatus(st);
      const has = valuesFromStatus(st);
      await pilotReportSettings(dev_, has, "usb").catch(() => undefined);
      // Sıradakiler sormadan işlenir (sahip kararı): önce ayarlar, sonra alanlar.
      const { wrote, problems } = st.role !== "tracker" ? { wrote: [], problems: [] } : await (async () => {
        setPhase("Sıradakiler işleniyor…");
        return deliverUsb(con!, {
          wanted: deviceSettings.get(dev_)?.wanted ?? {}, has,
          openSettings: deviceOps.open(dev_, "settings"), openFence: deviceOps.open(dev_, "fence"),
          reportSettings: (v) => pilotReportSettings(dev_, v, "usb").catch(() => undefined),
          recordFence: (sum, zones) => pilotRecordFence(dev_, { digest: sum.digest, areas: sum.areas, vertices: sum.vertices, zones, appliedAt: Date.now() }),
          result: (op, state, note) => pilotOpResult(op, dev_, state, "usb", note).catch(() => undefined),
        });
      })();
      if (problems.length && alive.current) setError(problems.join(" "));
      if (wrote.length) {
        setPhase("Yeniden okunuyor…");
        st = buildStatus(await readRawUsb(con), "usb", Date.now());
        if (alive.current) { setStatus(st); setSynced(`Tasmaya işlendi (tasma onayladı): ${wrote.join(", ")}.`); }
      }
    } finally {
      if (con) await con.close();
    }
  }

  async function read(): Promise<void> {
    setError("");
    try {
      if ("usb" in target) await readUsb(); else await readBle();
      setPhase("");
    } catch (e) {
      if (link.current) { await link.current.close(); link.current = null; }
      if (alive.current) { setPhase(""); setError(errText(e)); }
    }
  }

  useEffect(() => {
    alive.current = true;
    void read();
    return () => { alive.current = false; void link.current?.close(); link.current = null; };
  }, []); // eslint-disable-line react-hooks/exhaustive-deps

  const a = id ? m.herd.find((x) => x.device && x.device.toUpperCase() === id.toUpperCase()) ?? null : null;
  const plan = id ? fences.find((x) => x.device.toUpperCase() === id.toUpperCase()) ?? null : null;
  const st = status;
  const title = a ? a.name : id ? bleName(id) : "ORUN cihazı";
  const isGateway = st?.role === "gateway";
  const waitingKeys = id && st ? pendingKeys({ ...(deviceSettings.get(id) ?? { wanted: {}, wantedAt: null, reportedAt: null, reportedVia: null }),
    reported: valuesFromStatus(st) }) : [];

  return (
    <Dialog title={title} onClose={onClose}>
      <div className="stack" style={{ padding: "0 20px 16px" }}>
        <span className="sm row" style={{ gap: 6 }}>
          {"usb" in target ? <Usb size={16} /> : <Bluetooth size={16} />}
          {"usb" in target ? "USB" : "Bluetooth"}
          {st && ` · ${fmt.clock(st.at)}'de okundu`}
          {phase && ` · ${phase}`}
        </span>
        {error && <p role="alert" className="sm t-aging" style={{ margin: 0 }}>{error}</p>}

        {st && (
          <div className="card" style={{ margin: 0 }}>
            {isGateway && <KV k="Cihaz">Gateway</KV>}
            {st.role === "relay" && <KV k="Cihaz">Aktarıcı</KV>}
            <KV k="Pil">{st.battery ? (st.battery.valid
              ? `${st.battery.percent !== null ? `%${st.battery.percent} · ` : ""}${fmt.dec(st.battery.mv / 1000, 2)} V${batteryWord[st.battery.state] ? ` · ${batteryWord[st.battery.state]}` : ""}`
              : "Ölçülmedi") : "—"}</KV>
            {!isGateway && <KV k="Rapor aralığı">{st.interval ? <>
              {fmt.period(st.interval.requestedS)}{st.interval.stored ? "" : " (varsayılan)"}
              {st.interval.effectiveS !== null && st.interval.effectiveS !== st.interval.requestedS ? ` · şu an ${fmt.period(st.interval.effectiveS)}` : ""}
            </> : "—"}</KV>}
            {st.services && <KV k="Konum takibi">{svcWord(st.services.tracking, st.services.trackingWhy)}</KV>}
            {st.gnss && !isGateway && <KV k="GPS">{gnssWord(st.gnss.state)}{st.gnss.lastTtffMs ? ` · son konumu ${Math.round(st.gnss.lastTtffMs / 1000)} sn'de buldu` : ""}</KV>}
            {st.fence && !isGateway && <KV k="Alanlar">{st.fence.state === "configured"
              ? `${st.fence.areas} alan · ${st.fence.vertices} köşe${st.fence.position === "inside" ? " · içeride" : st.fence.position === "outside" ? " · dışarıda" : ""}`
              : st.fence.state === "clear" ? "Alan yok" : "Okunamadı"}</KV>}
            {st.fence && plan?.record && !isGateway && (
              <KV k="Hesaptaki kayıt">{plan.record.areas === st.fence.areas && plan.record.vertices === st.fence.vertices
                ? "Aynı" : `Farklı (hesapta ${plan.record.areas} alan · ${plan.record.vertices} köşe)`}</KV>
            )}
            {st.services && <KV k="Aktarma">{svcWord(st.services.relay)}</KV>}
            {st.storage && <KV k="Kayıt hafızası">{`${fmt.num(st.storage.count)} / ${fmt.num(st.storage.capacity)}`}</KV>}
            {st.firmware && <KV k="Yazılım">{st.firmware}</KV>}
            {st.uptimeMs !== null && <KV k="Açık kaldığı süre">{fmt.duration(st.uptimeMs)}</KV>}
            {st.missing.length > 0 && <p className="fine" style={{ padding: "6px 16px 10px", margin: 0 }}>Yanıt gelmeyen: {st.missing.join(", ")}</p>}
          </div>
        )}

        <div className="btns">
          <button className="btn" disabled={!!phase} onClick={() => { void read(); }}><ArrowClockwise size={18} />Yenile</button>
          {a && !fromAnimal && <button className="btn primary" onClick={() => { onClose(); nav.push({ to: "animal", id: a.id }); }}>Hayvanın kartı</button>}
          {!a && st && !isGateway && onAdd && "usb" in target && <button className="btn primary" onClick={onAdd}>Hesabına ekle</button>}
          {isGateway && onBridge && <button className="btn primary" onClick={onBridge}>Köprü olarak bağlan</button>}
        </div>
        {!a && st && !isGateway && !("usb" in target) && <p className="fine" style={{ margin: 0 }}>Bu tasma hesabında yok. Eklemek için USB ile bağla (Sürü → "+").</p>}
        {synced && <p className="sm" role="status" style={{ margin: 0 }}>{synced}</p>}
        {st && !isGateway && waitingKeys.length > 0 && !("usb" in target) && (
          <p className="fine" style={{ margin: 0 }}>{waitingKeys.length} ayar sırada. Bluetooth'tan yazma tasma yazılımı güncellenince; şimdilik USB ile bağlan.</p>
        )}
      </div>
    </Dialog>
  );
}

