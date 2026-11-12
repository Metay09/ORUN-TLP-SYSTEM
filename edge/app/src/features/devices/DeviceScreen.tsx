// 4b: bir ORUN cihazının ekranı (sahip kararı 2026-11-02). Her tür cihaz için
// aynı: üstte durum ve bağlan, altında o cihazda olan ayarlar. Ayar hesaba
// kaydedilir, cihaza bağlanınca sormadan yazılır; her ayarın yanında cihazdaki
// değer ve teslim durumu. Gateway'in ayarı yok (adı ve durumu var).
import { useEffect, useState, useSyncExternalStore } from "react";
import { Bluetooth, CaretRight, LinkBreak, MapPin, Pulse, Usb, X } from "@phosphor-icons/react";
import { settingRows, type SettingRow } from "../../app/deviceSettings.ts";
import { bleName } from "../../app/deviceNames.ts";
import { cap, fmt, tr } from "../../app/i18n/tr.ts";
import { useBack, useNav } from "../../app/nav.ts";
import type { FarmGateway } from "../../app/ownerAccountClient.ts";
import {
  deviceNames, deviceSettings, pilotAccountStatus, pilotGateways, pilotWantSettings,
} from "../../app/ownerAccountPilotRuntime.ts";
import { useApp } from "../../app/store.ts";
import { Badge, Bar, Card, Cell, Dialog, KV, Screen, Section, Toggle } from "../../ds/components.tsx";
import { trackerSession } from "../../app/trackerSession.ts";
import { BatteryLife, useConfiguredInterval } from "./BatteryLife.tsx";
import { startPosition, usePosition } from "../map/position.ts";
import { TrackerLiveDialog, type LiveTarget } from "../herd/TrackerLive.tsx";
import { NameDialog } from "../network/NameDialog.tsx";
import { gatewayFacts, gatewayLine } from "../network/words.ts";
import { ApplyFenceDialog, stateLine, useQueuedFences, useTrackerFences } from "../zones/ApplyFence.tsx";
import { OpSteps, opSub, useOps } from "./Ops.tsx";
import { KIND_WORD, operationSections } from "../../app/deviceOps.ts";

export function useDeviceSettings() {
  return useSyncExternalStore((fn) => deviceSettings.subscribe(fn), () => deviceSettings.snapshot());
}

export function rowSub(r: SettingRow): string {
  if (r.state === "waiting")
    return `Sırada · LoRa'dan ya da bağlanınca işlenir${r.onDevice !== null ? ` · tasmada ${r.def.word(r.onDevice)}` : ""}`;
  if (r.state === "device") return "Cihazda";
  if (r.state === "observed") return "Cihazın kayıtlarına göre";
  return "Cihazdan henüz okunmadı";
}

export function DeviceScreen({ id, ble }: { id: string; ble?: string }) {
  const m = useApp();
  const nav = useNav();
  const names = useSyncExternalStore((fn) => deviceNames.subscribe(fn), () => deviceNames.snapshot());
  const all = useDeviceSettings();
  const fences = useTrackerFences();
  const [gateways, setGateways] = useState<FarmGateway[] | null>(null);
  // Opened from the nearby list: connect right away (an open link is reused).
  const [live, setLive] = useState<LiveTarget | null>(() => (ble ? { ble } : null));
  const [editing, setEditing] = useState<SettingRow | null>(null);
  const [sending, setSending] = useState(false);
  const [naming, setNaming] = useState(false);
  const [marking, setMarking] = useState(false);
  const pos = usePosition();
  const live_ = useSyncExternalStore(trackerSession.subscribe, trackerSession.snapshot);
  useBack(() => nav.pop());
  useEffect(() => {
    if (marking && pos.pos) { m.actions.markStation({ lat: pos.pos.lat, lon: pos.pos.lon }); setMarking(false); }
  }, [marking, pos.pos, m.actions]);

  const up = id.toUpperCase();
  const a = m.herd.find((x) => x.device?.toUpperCase() === up) ?? null;
  const dev = m.devices.find((d) => d.id.toUpperCase() === up) ?? null;
  const gw = gateways?.find((g) => g.gateway.toUpperCase() === up) ?? null;
  const isGateway = !a && (!!gw || m.station.id?.toUpperCase() === up);
  const fence = fences.find((x) => x.device.toUpperCase() === up) ?? null;
  const queuedFence = useQueuedFences();
  const myOps = operationSections(useOps().filter((r) => r.device === up));
  // Hiç okunmadıysa cihazın kendi kayıtlarından görülen: rapor aralığı, konum takibi.
  const observed = {
    ...(a?.periodS ? { interval_s: a.periodS } : {}),
    ...(dev?.services.some((s) => s.key === "location" && s.state === "running") ? { tracking: true } : {}),
  };
  const rows = settingRows(all[up], observed);
  const title = a ? a.name : names[up] ?? bleName(id);
  const interval = useConfiguredInterval(a, up);
  const st = m.station;
  // This phone's own link to this device: the gateway bridge, or the tracker's Bluetooth session.
  const bridged = st.link !== "none" && st.id?.toUpperCase() === up;
  const session = !isGateway && live_ && live_.link.alive && live_.device === up ? live_ : null;

  useEffect(() => {
    if (a || !pilotAccountStatus()) return;
    let alive = true;
    pilotGateways().then((g) => { if (alive) setGateways(g); }).catch(() => undefined);
    return () => { alive = false; };
  }, [a]);

  return (
    <Screen title={title} onBack={() => nav.pop()}>
      <div className="stack" style={{ padding: "0 16px" }}>
        <span className="sm">
          {isGateway ? "Gateway" : "Tasma"} · {bleName(id)}
          {a && <> · <button className="lnk" onClick={() => nav.push({ to: "animal", id: a.id })}>{a.name} kartı</button></>}
        </span>
      </div>

      {bridged && <>
        <Section title="Bu telefona bağlı" info={tr.station.background} />
        <Card>
          <div className="stack" style={{ padding: 16 }}>
            <div className="row" style={{ gap: 12 }}>
              <span className={"ic " + (st.phase === "connected" ? "fresh" : "aging")}>{st.link === "usb" ? <Usb size={22} /> : <Bluetooth size={22} />}</span>
              <div className="stack grow" style={{ gap: 2 }}>
                <span className="strong">{st.phase === "connected" ? `${st.link === "usb" ? "USB" : "Bluetooth"} ile bağlı` : st.phase === "waiting" ? tr.station.waiting : tr.station.searching}</span>
                <span className="sm">Kayıtlar bu telefona akıyor, sunucuya buradan gidiyor</span>
              </div>
            </div>
            {st.capacity !== null && <>
              <div className="row" style={{ justifyContent: "space-between" }}><span className="sm">{tr.station.memory}</span><span className="sm strong">{fmt.num(st.held ?? 0)} / {fmt.num(st.capacity)}</span></div>
              <Bar ratio={(st.held ?? 0) / st.capacity} tone={(st.held ?? 0) / st.capacity >= 0.8 ? "bad" : (st.held ?? 0) / st.capacity >= 0.5 ? "warn" : undefined} />
            </>}
            <div className="row" style={{ justifyContent: "space-between" }}>
              <span className="sm">{tr.station.position}</span>
              <span className="sm strong">{st.position ? fmt.coords(st.position.lat, st.position.lon) : tr.station.positionNone}</span>
            </div>
            <div className="btns" style={{ padding: 0 }}>
              <button className="btn small" onClick={() => { startPosition(); setMarking(true); }} disabled={marking}><MapPin size={18} />{marking ? "Konum alınıyor…" : "Yeri burası"}</button>
              {st.position && <button className="btn small quiet" onClick={() => m.actions.markStation(null)}><X size={16} />{tr.station.clearPosition}</button>}
            </div>
            <button className="btn danger wide" onClick={() => { void m.source.disconnect(); }}><LinkBreak size={18} />{tr.station.disconnect}</button>
          </div>
        </Card>
      </>}
      {session && <>
        <Section title="Bu telefona bağlı" info="Bağlantı bu ekrandan çıkınca da açık kalır; tasmanın Bluetooth'u bağlıyken kapanmaz. İşin bitince 'Bağlantıyı kes'e bas." />
        <Card>
          <div className="stack" style={{ padding: 16 }}>
            <div className="row" style={{ gap: 12 }}>
              <span className="ic fresh"><Bluetooth size={22} /></span>
              <div className="stack grow" style={{ gap: 2 }}>
                <span className="strong">Bluetooth ile bağlı</span>
                <span className="sm">{cap(fmt.since(session.since, Math.max(m.now, session.since)))} açık{session.status ? ` · son okuma ${fmt.clock(session.status.at)}` : ""}</span>
              </div>
            </div>
            <div className="btns" style={{ padding: 0 }}>
              <button className="btn small primary" onClick={() => setLive({ ble: session.bleId })}><Pulse size={18} />Canlı durum</button>
              <button className="btn small danger" onClick={() => { void trackerSession.close(); }}><LinkBreak size={18} />Kes</button>
            </div>
          </div>
        </Card>
      </>}

      {!(isGateway && !gw && bridged) && <Section title="Durum" info="Bağlan: Bluetooth ile canlı durumunu okur; bekleyen ayarlar ve alanlar sormadan yazılır (yalnız hesaba girmiş telefon, tasma onaylayınca işlenir). USB ile: tasmayı OTG kablosuyla bağla; aynısı kabloyla. Tasmanın Bluetooth'u açıldıktan sonra 10 dakika, sonra her rapordan sonra ayardaki süre kadar açık kalır." />}
      {!(isGateway && !gw && bridged) && <Card>
        {isGateway ? (
          gw ? <>
            <KV k="Durum">{gatewayLine(gw, m.now)}</KV>
            <KV k="Bellek">{gatewayFacts(gw)}</KV>
          </> : <p className="sm" style={{ padding: "14px 16px", margin: 0 }}>Hesaba giriş yapınca gateway'in durumu görünür.</p>
        ) : <>
          <KV k={tr.device.lastContact}>{dev?.lastTime ? `${fmt.clock(dev.lastTime)} · ${fmt.ago(dev.lastTime, m.now)}` : "—"}</KV>
          <KV k={tr.animal.battery}>{dev?.batteryPower === "NO_BATTERY" ? "Pil takılı değil · USB"
            : dev && dev.batteryPercent !== null ? `%${dev.batteryPercent}${dev.batteryPower === "CHARGING" ? " · şarj oluyor" : dev.batteryPower === "FULL" ? " · dolu" : ""}${dev.batteryUsb ? " · USB" : ""}${dev.batteryMv ? ` · ${fmt.dec(dev.batteryMv / 1000, 2)} V` : ""}` : "—"}</KV>
          <KV k={tr.device.signal}>{dev && dev.rssi !== null ? tr.device.signalLine(dev.rssi, dev.snr) : tr.device.signalNone}</KV>
          {dev?.link && <KV k="Yol">{tr.device.link[dev.link]}</KV>}
          {dev && dev.medianDelayS !== null && <KV k={tr.device.delay}>{dev.medianDelayS < 60 ? `${dev.medianDelayS} sn` : fmt.duration(dev.medianDelayS * 1000)}</KV>}
          {dev && <KV k={tr.device.records}>{fmt.num(dev.records)}</KV>}
          {dev && dev.services.length > 0 && (
            <div className="row" style={{ gap: 6, flexWrap: "wrap", padding: "8px 16px 6px" }}>
              {dev.services.map((s) => (
                <Badge key={s.key} tone={s.state === "running" ? "fresh" : "silent"}>{tr.device.service[s.key]} · {tr.device.serviceState[s.state]}</Badge>
              ))}
            </div>
          )}
          {!session && <div className="btns" style={{ paddingTop: 6 }}>
            <button className="btn small primary" type="button" onClick={() => setLive({ find: id })}><Bluetooth size={18} />Bağlan</button>
            <button className="btn small" type="button" onClick={() => setLive({ usb: true })}><Usb size={18} />USB ile</button>
          </div>}
        </>}
      </Card>}

      {!isGateway && <>
        <Section title="Ayarlar" info="Ayar hesaba kaydedilir; tasmaya bağlanınca (USB ya da Bluetooth) sormadan yazılır. Tasma yazdığını onaylayınca 'Cihazda' olur. Bütün sürüye birden: Ayarlar → Tasma ayarları." />
        <Card>
          {rows.map((r) => (
            <Cell key={r.def.key} title={r.def.title} wrapSub
              sub={rowSub(r)} subTone={r.state === "waiting" ? "aging" : undefined}
              trail={<span className="sm row" style={{ gap: 4 }}>{r.value !== null ? r.def.word(r.value) : "—"}<CaretRight size={16} color="var(--mute)" /></span>}
              onClick={() => setEditing(r)} />
          ))}
          <Cell title="Alanlar" wrapSub
            sub={fence ? (queuedFence.has(up) ? "Sırada · LoRa'dan ya da bağlanınca işlenir" : stateLine(fence)) : "Bu tasmaya gidecek alan yok"}
            subTone={fence && (fence.state === "stale" || fence.state === "never" || fence.state === "clear") ? "aging" : undefined}
            trail={<CaretRight size={16} color="var(--mute)" />}
            onClick={() => (fence && (fence.state === "stale" || fence.state === "never" || fence.state === "clear") && !queuedFence.has(up) ? setSending(true) : nav.push({ to: "zones" }))} />
          {fence?.record && fence.record.zones.length > 0 && (
            <KV k="Tasmadaki alanlar">{fence.record.zones.map((z) => z.name).join(", ")}</KV>
          )}
        </Card>
      </>}

      {!isGateway && <BatteryLife device={up} intervalS={interval} series={a?.batteryTrail ?? []} nowPct={a?.batteryPercent ?? dev?.batteryPercent ?? null} />}

      {!isGateway && myOps.active.length > 0 && <>
        <Section title="İşlemler" info="Her ayar kaydı ve 'Tasmalara uygula' bir işlemdir. Sıradakiler tasmaya bağlanınca (şimdilik USB) kendiliğinden işlenir; 'Tasmaya işlendi' yalnız tasma onaylayınca yazılır." />
        <Card>
          {myOps.active.slice(0, 5).flatMap((g) => g.targets).map((t) => (
            <Cell key={t.op} title={KIND_WORD[t.kind]} wrapSub sub={<>{opSub(t)}<OpSteps t={t} /></>}
              subTone={t.state === "rejected" ? "alarm" : t.state === "applied" || t.state === "same" ? "fresh" : undefined} />
          ))}
        </Card>
      </>}

      {!isGateway && myOps.history.length > 0 && <details className="tech">
        <summary>İşlem geçmişi ({myOps.history.length})</summary>
        <Card>
          {myOps.history.slice(0, 20).flatMap((g) => g.targets).map((t) => (
            <Cell key={t.op + t.device} title={KIND_WORD[t.kind]} sub={opSub(t)} />
          ))}
        </Card>
      </details>}

      {isGateway && <>
        <Section title="Ad" />
        <Card>
          <Cell title={names[up] ?? "Ad verilmedi"} sub="Uygulamada bu adla görünür" trail={<CaretRight size={16} color="var(--mute)" />} onClick={() => setNaming(true)} />
        </Card>
      </>}

      <details className="tech">
        <summary>{tr.common.tech}</summary>
        <Card><KV k="Kimlik"><code>{id}</code></KV></Card>
      </details>

      {editing && <EditSetting device={up} row={editing} onClose={() => setEditing(null)} />}
      {live && <TrackerLiveDialog device={id} target={live} fromAnimal={!!a} onClose={() => setLive(null)} />}
      {sending && <ApplyFenceDialog only={id} onClose={() => setSending(false)} />}
      {naming && <NameDialog device={id} kind="gateway" current={names[up] ?? null} onClose={() => setNaming(false)} />}
    </Screen>
  );
}

/** One setting for one device (whole herd: Ayarlar → Tasma ayarları). */
function EditSetting({ device, row, onClose }: { device: string; row: SettingRow; onClose: () => void }) {
  const [value, setValue] = useState<number | boolean | null>(row.value);
  const [error, setError] = useState("");
  const [busy, setBusy] = useState(false);
  useBack(onClose);
  const def = row.def;
  const choices = def.choices && typeof row.onDevice === "number" && !def.choices.includes(row.onDevice)
    ? [...def.choices, row.onDevice].sort((x, y) => x - y) : def.choices;

  async function save(): Promise<void> {
    if (value === null) return;
    setBusy(true); setError("");
    try { await pilotWantSettings([device], { [def.key]: value }); onClose(); }
    catch (e) { setError(e instanceof Error ? e.message : "Kaydedilemedi."); }
    finally { setBusy(false); }
  }

  return (
    <Dialog title={def.title} onClose={onClose}>
      <div className="stack" style={{ padding: "0 20px 16px" }}>
        <p className="sm" style={{ margin: 0 }}>{def.help}</p>
        {def.kind === "choice" ? (
          <select className="inp" aria-label={def.title} value={typeof value === "number" ? value : ""}
            onChange={(e) => setValue(Number(e.target.value))}>
            {value === null && <option value="">—</option>}
            {choices!.map((c) => <option key={c} value={c}>{def.word(c)}{c === row.onDevice ? " (cihazda)" : ""}</option>)}
          </select>
        ) : (
          <div className="row" style={{ justifyContent: "space-between" }}>
            <span className="sm">{def.word(value === true)}</span>
            <Toggle on={value === true} label={def.title} onChange={(v) => setValue(v)} />
          </div>
        )}
        <p className="fine" style={{ margin: 0 }}>Kaydedince hesaba yazılır; tasmaya bağlanınca (USB ya da Bluetooth) sormadan gider.</p>
        <div className="btns" style={{ padding: 0 }}>
          <button className="btn" onClick={onClose}>Vazgeç</button>
          <button className="btn primary" disabled={busy || value === null || value === row.value} onClick={() => { void save(); }}>Kaydet</button>
        </div>
        {error && <p className="sm t-aging" role="alert" style={{ margin: 0 }}>{error}</p>}
      </div>
    </Dialog>
  );
}
