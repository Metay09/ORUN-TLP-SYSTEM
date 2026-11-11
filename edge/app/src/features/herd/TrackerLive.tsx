// 4a: tasmaya dokununca canlı bağlanır, durumunu okur. Bluetooth ve USB aynı
// kartı gösterir (aynı soruların aynı yanıtları).
import { useEffect, useRef, useState, useSyncExternalStore } from "react";
import { ArrowClockwise, Bluetooth, LinkBreak, Usb } from "@phosphor-icons/react";
import { noRoleMessage, probeUsbDevice } from "../../app/addTracker.ts";
import { clearFence } from "../../app/fenceUsb.ts";
import { bleClearFence } from "../../app/bleWrite.ts";
import { bleName } from "../../app/deviceNames.ts";
import { fmt } from "../../app/i18n/tr.ts";
import { collarStorageReadings } from "../../app/collarStorageReadings.ts";
import { useBack, useNav } from "../../app/nav.ts";
import { useApp } from "../../app/store.ts";
import {
  batteryWord, buildStatus, gnssWord, readRawUsb, svcWord, type TrackerStatus,
} from "../../app/trackerStatus.ts";
import { Dialog, KV } from "../../ds/components.tsx";
import { pendingKeys, valuesFromStatus, bleWindowWord } from "../../app/deviceSettings.ts";
import { deliverBle, deliverUsb } from "../../app/deliver.ts";
import { deviceOps, deviceSettings, pilotOpResult, pilotRecordFence, pilotReportSettings, writeKeys } from "../../app/ownerAccountPilotRuntime.ts";
import { TrackerBle } from "../../platform/trackerBle.ts";
import { trackerSession, usbTracker } from "../../app/trackerSession.ts";
import { UsbConsole } from "../../platform/usbConsole.ts";
import { useTrackerFences } from "../zones/ApplyFence.tsx";
import { stillWanted } from "../../app/fencePlan.ts";

/** ble: a device from the scan list; find: look for this ID's Bluetooth name; usb: the cable. */
export type LiveTarget = { ble: string } | { find: string } | { usb: true };

const BT_OFF = "Tasmanın Bluetooth'u kapalı olabilir. Tasma açıldıktan sonra 10 dakika açık kalır, sonra her rapordan sonra kısa süre açılır: biraz bekleyip tekrar dene, ya da tasmayı kapatıp aç. Ya da USB ile bağlan.";

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

  const rememberStorage = (deviceId: string | null, st: TrackerStatus) => {
    if (alive.current && deviceId && st.role === "tracker" &&
        st.storage?.ready) {
      collarStorageReadings.note(deviceId, st.storage.count,
                                 st.storage.capacity, st.at, st.via);
    }
  };

  /** The open zone op, only if it is still what the account wants; a stale one is closed, not written. */
  async function fenceOpFor(dev: string, via: "ble" | "usb") {
    const op = deviceOps.open(dev, "fence");
    const plan = fences.find((x) => x.device.toUpperCase() === dev.toUpperCase()) ?? null;
    const ok = stillWanted(op, plan);
    if (op && !ok) {
      await pilotOpResult(op.op, dev, "rejected", via, "Alanlar sonradan değişti; eski liste gönderilmedi").catch(() => undefined);
      if (alive.current) setError("Sıradaki alan işlemi eskimişti (alanlar sonradan değişti); tasmaya yazılmadı. Gerekirse Alanlar → Tasmalara uygula.");
    }
    return ok;
  }

  async function readBle(): Promise<void> {
    if (!link.current || !link.current.alive) {
      link.current = null;
      let bleId = "ble" in target ? target.ble : null;
      if ("find" in target) {
        // Already connected to this tracker: reuse the open link, no new scan.
        bleId = trackerSession.find(target.find)?.bleId ?? null;
        if (!bleId) {
          setPhase("Tasma aranıyor (Bluetooth)…");
          bleId = await TrackerBle.find(target.find);
        }
        if (!bleId) throw new Error(BT_OFF);
      }
      if (!trackerSession.find(id, bleId)) setPhase("Bluetooth ile bağlanıyor…");
      link.current = await trackerSession.open(bleId!, id, TrackerBle.open);
    }
    setPhase("Okunuyor…");
    const t = link.current;
    let st = buildStatus(await t.readRaw(), "ble", Date.now());
    trackerSession.note(t, st, id);
    if (alive.current) { setStatus(st); rememberStorage(id, st); }
    if (!id || st.role !== "tracker") return;
    const dev_ = id.toUpperCase();
    const has = valuesFromStatus(st);
    await pilotReportSettings(dev_, has, "ble").catch(() => undefined);
    // 4c-1b/4c-2: sıradaki ayarlar ve alanlar Bluetooth'tan da sormadan
    // yazılır (hesaptaki telefon, tasmanın imzalı onayıyla).
    setPhase("Sıradakiler işleniyor…");
    const { wrote, problems } = await deliverBle(t.exchange, writeKeys.get, dev_, {
      wanted: deviceSettings.get(dev_)?.wanted ?? {}, has,
      openSettings: deviceOps.open(dev_, "settings"), openFence: await fenceOpFor(dev_, "ble"),
      reportSettings: (v) => pilotReportSettings(dev_, v, "ble").catch(() => undefined),
      recordFence: (sum, zones) => pilotRecordFence(dev_, { digest: sum.digest, areas: sum.areas, vertices: sum.vertices, zones, appliedAt: Date.now() }),
      result: (op, state, note) => pilotOpResult(op, dev_, state, "ble", note).catch(() => undefined),
    });
    if (problems.length && alive.current) setError(problems.join(" "));
    if (wrote.length) {
      setPhase("Yeniden okunuyor…");
      st = buildStatus(await t.readRaw(), "ble", Date.now());
      trackerSession.note(t, st, id);
      if (alive.current) { setStatus(st); rememberStorage(id, st); setSynced(`Tasmaya işlendi (tasma onayladı): ${wrote.join(", ")}.`); }
    }
  }

  async function readUsb(): Promise<void> {
    let con: UsbConsole | null = null;
    try {
      setPhase("USB açılıyor…");
      con = await UsbConsole.open();
      const dev = await probeUsbDevice(con);
      if (dev.kind === "gateway") throw new Error("USB'deki cihaz bir gateway. Gateway'e Bağlantı → USB'den bağlan.");
      if (dev.kind === "unassigned") throw new Error(noRoleMessage("UNASSIGNED") + " Bağlantı → \"USB ile bağlı cihaza bağlan\"dan kur.");
      if (dev.kind === "conflict") throw new Error(noRoleMessage("CONFLICT"));
      if (alive.current) setId(dev.report.device);
      setPhase("Okunuyor…");
      const dev_ = dev.report.device.toUpperCase();
      let st = buildStatus(await readRawUsb(con), "usb", Date.now());
      usbTracker.note(dev_, st.at);
      if (alive.current) { setStatus(st); rememberStorage(dev_, st); }
      const has = valuesFromStatus(st);
      await pilotReportSettings(dev_, has, "usb").catch(() => undefined);
      // Sıradakiler sormadan işlenir (sahip kararı): önce ayarlar, sonra alanlar.
      const { wrote, problems } = st.role !== "tracker" ? { wrote: [], problems: [] } : await (async () => {
        setPhase("Sıradakiler işleniyor…");
        return deliverUsb(con!, {
          wanted: deviceSettings.get(dev_)?.wanted ?? {}, has,
          openSettings: deviceOps.open(dev_, "settings"), openFence: await fenceOpFor(dev_, "usb"),
          reportSettings: (v) => pilotReportSettings(dev_, v, "usb").catch(() => undefined),
          recordFence: (sum, zones) => pilotRecordFence(dev_, { digest: sum.digest, areas: sum.areas, vertices: sum.vertices, zones, appliedAt: Date.now() }),
          result: (op, state, note) => pilotOpResult(op, dev_, state, "usb", note).catch(() => undefined),
        });
      })();
      if (problems.length && alive.current) setError(problems.join(" "));
      if (wrote.length) {
        setPhase("Yeniden okunuyor…");
        st = buildStatus(await readRawUsb(con), "usb", Date.now());
        if (alive.current) { setStatus(st); rememberStorage(dev_, st); setSynced(`Tasmaya işlendi (tasma onayladı): ${wrote.join(", ")}.`); }
      }
    } finally {
      if (con) await con.close();
    }
  }

  /** The tracker holds zones the account no longer gives it: remove them (USB only, explicit tap). */
  async function clearUsb(): Promise<void> {
    let con: UsbConsole | null = null;
    setError(""); setPhase("Tasmadaki alanlar siliniyor…");
    try {
      con = await UsbConsole.open();
      const dev = await probeUsbDevice(con);
      if (dev.kind !== "tracker" || (id && dev.report.device.toUpperCase() !== id.toUpperCase()))
        throw new Error("USB'deki cihaz bu tasma değil.");
      const dev_ = dev.report.device.toUpperCase();
      const sum = await clearFence(con);
      await pilotRecordFence(dev_, { digest: sum.digest, areas: 0, vertices: 0, zones: [], appliedAt: Date.now() });
      const st = buildStatus(await readRawUsb(con), "usb", Date.now());
      if (alive.current) { setStatus(st); setSynced("Tasmadaki alanlar silindi (tasma onayladı)."); }
    } catch (e) {
      if (alive.current) setError(errText(e));
    } finally {
      if (con) await con.close();
      if (alive.current) setPhase("");
    }
  }

  /** Same over Bluetooth: the signed "remove all zones" (tasma firmware with op 5). */
  async function clearBle(): Promise<void> {
    const t = link.current;
    if (!t || !id) return;
    setError(""); setPhase("Tasmadaki alanlar siliniyor…");
    try {
      const sum = await bleClearFence(t.exchange, writeKeys.get, id);
      await pilotRecordFence(id.toUpperCase(), { digest: sum.digest, areas: 0, vertices: 0, zones: [], appliedAt: Date.now() });
      const st = buildStatus(await t.readRaw(), "ble", Date.now());
      trackerSession.note(t, st, id);
      if (alive.current) { setStatus(st); setSynced("Tasmadaki alanlar silindi (tasma onayladı)."); }
    } catch (e) {
      if (alive.current) setError(errText(e));
    } finally {
      if (alive.current) setPhase("");
    }
  }

  async function read(): Promise<void> {
    setError("");
    try {
      if ("usb" in target) await readUsb(); else await readBle();
      setPhase("");
    } catch (e) {
      // A dropped link is already gone from the session; a slow answer keeps it open.
      if (link.current && !link.current.alive) link.current = null;
      if (alive.current) { setPhase(""); setError(errText(e)); }
    }
  }

  useEffect(() => {
    alive.current = true;
    void read();
    // Closing this card does NOT disconnect (owner 2026-11-11): the Bluetooth
    // link stays open until "Bağlantıyı kes" or until the tracker goes away.
    return () => { alive.current = false; link.current = null; };
  }, []); // eslint-disable-line react-hooks/exhaustive-deps

  const live = useSyncExternalStore(trackerSession.subscribe, trackerSession.snapshot);
  const session = !("usb" in target) && live && link.current && live.link === link.current ? live : null;
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
          {"usb" in target ? "USB" : session ? "Bluetooth · bağlı" : "Bluetooth"}
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
            {!isGateway && typeof st.bleWindowS === "number" && <KV k="Rapordan sonra Bluetooth">{bleWindowWord(st.bleWindowS)}</KV>}
            {st.storage && <KV k="Kayıt hafızası">{`${fmt.num(st.storage.count)} / ${fmt.num(st.storage.capacity)}`}</KV>}
            {st.firmware && <KV k="Yazılım">{st.firmware}</KV>}
            {st.uptimeMs !== null && <KV k="Açık kaldığı süre">{fmt.duration(st.uptimeMs)}</KV>}
            {st.missing.length > 0 && <p className="fine" style={{ padding: "6px 16px 10px", margin: 0 }}>Yanıt gelmeyen: {st.missing.join(", ")}</p>}
          </div>
        )}

        <div className="btns">
          <button className="btn" disabled={!!phase} onClick={() => { void read(); }}><ArrowClockwise size={18} />Yenile</button>
          {session && <button className="btn danger" onClick={() => { void trackerSession.close(); onClose(); }}><LinkBreak size={18} />Bağlantıyı kes</button>}
          {a && !fromAnimal && <button className="btn primary" onClick={() => { onClose(); nav.push({ to: "animal", id: a.id }); }}>Hayvanın kartı</button>}
          {!a && st && !isGateway && onAdd && "usb" in target && <button className="btn primary" onClick={onAdd}>Hesabına ekle</button>}
          {isGateway && onBridge && <button className="btn primary" onClick={onBridge}>Köprü olarak bağlan</button>}
        </div>
        {!a && st && !isGateway && !("usb" in target) && <p className="fine" style={{ margin: 0 }}>Bu tasma hesabında yok. Eklemek için USB ile bağla (Sürü → "+").</p>}
        {synced && <p className="sm" role="status" style={{ margin: 0 }}>{synced}</p>}
        {st && !isGateway && st.fence?.state === "configured" && (!plan || plan.zones.length === 0) && (
          <div className="stack" style={{ gap: 6 }}>
            <p className="sm t-aging" style={{ margin: 0 }}>Tasmada {st.fence.areas} alan var ama hesapta bu tasmaya alan yok. Tasma bu alanlara göre içeride/dışarıda karar veriyor.</p>
            <button className="btn danger" disabled={!!phase || (!("usb" in target) && !session)}
              onClick={() => { void ("usb" in target ? clearUsb() : clearBle()); }}>Tasmadaki alanları sil</button>
          </div>
        )}
        {st && !isGateway && (waitingKeys.length > 0 || !!deviceOps.open(id ?? "", "fence")) && !("usb" in target) && id && !writeKeys.has(id) && (
          <p className="fine" style={{ margin: 0 }}>Sırada bekleyen var. Bluetooth'tan yazmak için bu telefonda tasmanın yazma anahtarı yok: hesaba girip internete bağlanınca kendiliğinden gelir. Şimdilik USB ile bağlan.</p>
        )}
      </div>
    </Dialog>
  );
}

