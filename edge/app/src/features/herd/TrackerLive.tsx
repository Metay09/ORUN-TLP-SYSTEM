// 4a: tasmaya dokununca canlı bağlanır, durumunu okur. Bluetooth ve USB aynı
// kartı gösterir (aynı soruların aynı yanıtları).
import { useEffect, useRef, useState } from "react";
import { ArrowClockwise, Bluetooth, Usb } from "@phosphor-icons/react";
import { probeUsbDevice } from "../../app/addTracker.ts";
import { bleName } from "../../app/deviceNames.ts";
import { fmt } from "../../app/i18n/tr.ts";
import { useBack, useNav } from "../../app/nav.ts";
import { useApp } from "../../app/store.ts";
import {
  batteryWord, buildStatus, gnssWord, readRawUsb, svcWord, type TrackerStatus,
} from "../../app/trackerStatus.ts";
import { Dialog, KV, ToggleRow } from "../../ds/components.tsx";
import {
  applySetting, INTERVAL_CHOICES, intervalCommand, servicesCommand, type ServiceChoice,
} from "../../app/trackerSettings.ts";
import { TrackerBle } from "../../platform/trackerBle.ts";
import { UsbConsole } from "../../platform/usbConsole.ts";
import { useTrackerFences } from "../zones/SendFenceDialog.tsx";

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
    if (alive.current) setStatus(buildStatus(raw, "ble", Date.now()));
  }

  async function readUsb(): Promise<void> {
    let con: UsbConsole | null = null;
    try {
      setPhase("USB açılıyor…");
      con = await UsbConsole.open();
      const dev = await probeUsbDevice(con);
      if (dev.kind === "gateway") throw new Error("USB'deki cihaz bir gateway. Gateway'e Bağlantı → USB'den bağlan.");
      if (alive.current) setId(dev.report.device);
      setPhase("Okunuyor…");
      const raw = await readRawUsb(con);
      if (alive.current) setStatus(buildStatus(raw, "usb", Date.now()));
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
        {st && !isGateway && st.role !== null && ("usb" in target
          ? <UsbSettings device={id} status={st} onDone={() => { void read(); }} busy={!!phase} />
          : <p className="fine" style={{ margin: 0 }}>Ayar değiştirmek için USB ile bağlan; Bluetooth'tan ayar sonraki güncellemede.</p>)}
      </div>
    </Dialog>
  );
}

/** 4b: rapor aralığı ve hizmetler, USB ile. Yazılınca kart yeniden okunur. */
function UsbSettings({ device, status, onDone, busy }: { device: string | null; status: TrackerStatus; onDone: () => void; busy: boolean }) {
  const current = status.interval?.requestedS ?? null;
  const req = status.services?.requested ?? null;
  const [interval, setInterval_] = useState<number | null>(current);
  const [svc, setSvc] = useState<ServiceChoice>({
    tracking: req ? req.tracking : status.services?.tracking !== "off",
    relay: req ? req.relay : status.services?.relay === "on",
  });
  const [working, setWorking] = useState("");
  const [note, setNote] = useState("");
  const [error, setError] = useState("");
  const choices = current !== null && !INTERVAL_CHOICES.includes(current) ? [...INTERVAL_CHOICES, current].sort((x, y) => x - y) : INTERVAL_CHOICES;
  const svcChanged = !req || req.tracking !== svc.tracking || req.relay !== svc.relay;
  const intervalChanged = interval !== null && interval !== current;

  async function save(): Promise<void> {
    setError(""); setNote("");
    let con: UsbConsole | null = null;
    const done: string[] = [];
    try {
      setWorking("USB açılıyor…");
      con = await UsbConsole.open();
      const dev = await probeUsbDevice(con);
      if (dev.kind !== "tracker" || (device && dev.report.device.toUpperCase() !== device.toUpperCase()))
        throw new Error("USB'deki cihaz değişmiş; kartı kapatıp yeniden bağlan.");
      if (intervalChanged) {
        setWorking("Rapor aralığı yazılıyor…");
        await applySetting(con, intervalCommand(interval!));
        done.push(`rapor aralığı ${fmt.period(interval!)}`);
      }
      if (svcChanged) {
        setWorking("Hizmetler yazılıyor…");
        await applySetting(con, servicesCommand(svc));
        done.push("hizmetler");
      }
      setNote(done.length ? `Tasmaya yazıldı: ${done.join(", ")}. Kart yeniden okunuyor.` : "Değişiklik yok.");
    } catch (e) {
      setError(errText(e) + (done.length ? ` (Yazılan: ${done.join(", ")}.)` : ""));
    } finally {
      if (con) await con.close();
      setWorking("");
      if (done.length) onDone();
    }
  }

  return (
    <div className="stack">
      <span className="sm strong">Ayarlar</span>
      <div className="card" style={{ margin: 0 }}>
        <div className="kv">
          <span>Rapor aralığı</span>
          <select className="inp" style={{ width: "auto" }} aria-label="Rapor aralığı" value={interval ?? ""}
            disabled={!!working || busy} onChange={(e) => setInterval_(Number(e.target.value))}>
            {current === null && <option value="">—</option>}
            {choices.map((sec) => <option key={sec} value={sec}>{fmt.period(sec)}</option>)}
          </select>
        </div>
        <ToggleRow title="Konum takibi" sub="Kapalıyken tasma GPS'i açmaz, konum göndermez."
          on={svc.tracking} onChange={(v) => setSvc({ ...svc, tracking: v })} />
        <ToggleRow title="Aktarma" sub="Başka tasmaların kayıtlarını gateway'e iletir. Açıkken telsiz hep dinler, pil daha çok gider."
          on={svc.relay} onChange={(v) => setSvc({ ...svc, relay: v })} />
      </div>
      {!req && status.services && <p className="fine" style={{ margin: 0 }}>Hizmetler şu an otomatik (donanıma göre). Kaydedersen seçtiğin hizmetler sabitlenir.</p>}
      <button className="btn primary wide" disabled={!!working || busy || (!intervalChanged && !svcChanged)} onClick={() => { void save(); }}>
        {working || "Tasmaya yaz"}
      </button>
      {note && <p className="sm" role="status" style={{ margin: 0 }}>{note}</p>}
      {error && <p className="sm t-aging" role="alert" style={{ margin: 0 }}>{error}</p>}
    </div>
  );
}
