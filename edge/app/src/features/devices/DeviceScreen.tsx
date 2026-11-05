// 4b: bir ORUN cihazının ekranı (sahip kararı 2026-11-02). Her tür cihaz için
// aynı: üstte durum ve bağlan, altında o cihazda olan ayarlar. Ayar hesaba
// kaydedilir, cihaza bağlanınca sormadan yazılır; her ayarın yanında cihazdaki
// değer ve teslim durumu. Gateway'in ayarı yok (adı ve durumu var).
import { useEffect, useState, useSyncExternalStore } from "react";
import { Bluetooth, CaretRight, Usb } from "@phosphor-icons/react";
import { settingRows, type SettingRow } from "../../app/deviceSettings.ts";
import { bleName } from "../../app/deviceNames.ts";
import { fmt, tr } from "../../app/i18n/tr.ts";
import { useBack, useNav } from "../../app/nav.ts";
import type { FarmGateway } from "../../app/ownerAccountClient.ts";
import {
  deviceNames, deviceSettings, pilotAccountStatus, pilotGateways, pilotWantSettings,
} from "../../app/ownerAccountPilotRuntime.ts";
import { useApp } from "../../app/store.ts";
import { Badge, Card, Cell, Dialog, KV, Screen, Section, Toggle } from "../../ds/components.tsx";
import { TrackerLiveDialog, type LiveTarget } from "../herd/TrackerLive.tsx";
import { NameDialog } from "../network/NameDialog.tsx";
import { gatewayFacts, gatewayLine } from "../network/words.ts";
import { ApplyFenceDialog, stateLine, useQueuedFences, useTrackerFences } from "../zones/ApplyFence.tsx";
import { OpSteps, opSub, useOps } from "./Ops.tsx";
import { KIND_WORD } from "../../app/deviceOps.ts";

export function useDeviceSettings() {
  return useSyncExternalStore((fn) => deviceSettings.subscribe(fn), () => deviceSettings.snapshot());
}

export function rowSub(r: SettingRow): string {
  if (r.state === "waiting")
    return `Sırada · tasmaya bağlanınca işlenir${r.onDevice !== null ? ` · tasmada ${r.def.word(r.onDevice)}` : ""}`;
  if (r.state === "device") return "Cihazda";
  if (r.state === "observed") return "Cihazın kayıtlarına göre";
  return "Cihazdan henüz okunmadı";
}

export function DeviceScreen({ id }: { id: string }) {
  const m = useApp();
  const nav = useNav();
  const names = useSyncExternalStore((fn) => deviceNames.subscribe(fn), () => deviceNames.snapshot());
  const all = useDeviceSettings();
  const fences = useTrackerFences();
  const [gateways, setGateways] = useState<FarmGateway[] | null>(null);
  const [live, setLive] = useState<LiveTarget | null>(null);
  const [editing, setEditing] = useState<SettingRow | null>(null);
  const [sending, setSending] = useState(false);
  const [naming, setNaming] = useState(false);
  useBack(() => nav.pop());

  const up = id.toUpperCase();
  const a = m.herd.find((x) => x.device?.toUpperCase() === up) ?? null;
  const dev = m.devices.find((d) => d.id.toUpperCase() === up) ?? null;
  const gw = gateways?.find((g) => g.gateway.toUpperCase() === up) ?? null;
  const isGateway = !a && (!!gw || m.station.id?.toUpperCase() === up);
  const fence = fences.find((x) => x.device.toUpperCase() === up) ?? null;
  const queuedFence = useQueuedFences();
  const myOps = useOps().filter((r) => r.device === up && r.state !== "superseded").slice(0, 5);
  // Hiç okunmadıysa cihazın kendi kayıtlarından görülen: rapor aralığı, konum takibi.
  const observed = {
    ...(a?.periodS ? { interval_s: a.periodS } : {}),
    ...(dev?.services.some((s) => s.key === "location" && s.state === "running") ? { tracking: true } : {}),
  };
  const rows = settingRows(all[up], observed);
  const title = a ? a.name : names[up] ?? bleName(id);

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

      <Section title="Durum" info="Bağlan: Bluetooth ile canlı durumunu okur; bekleyen ayarlar ve alanlar sormadan yazılır (yalnız hesaba girmiş telefon, tasma onaylayınca işlenir). USB ile: tasmayı OTG kablosuyla bağla; aynısı kabloyla. Tasmanın Bluetooth'u açıldıktan sonra 10 dakika, sonra her rapordan sonra ayardaki süre kadar açık kalır." />
      <Card>
        {isGateway ? (
          gw ? <>
            <KV k="Durum">{gatewayLine(gw, m.now)}</KV>
            <KV k="Bellek">{gatewayFacts(gw)}</KV>
          </> : <p className="sm" style={{ padding: "14px 16px", margin: 0 }}>Hesaba giriş yapınca gateway'in durumu görünür.</p>
        ) : <>
          <KV k={tr.device.lastContact}>{dev?.lastTime ? `${fmt.clock(dev.lastTime)} · ${fmt.ago(dev.lastTime, m.now)}` : "—"}</KV>
          <KV k={tr.animal.battery}>{dev && dev.batteryPercent !== null ? `%${dev.batteryPercent}${dev.batteryMv ? ` · ${fmt.dec(dev.batteryMv / 1000, 2)} V` : ""}` : "—"}</KV>
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
          <div className="btns" style={{ paddingTop: 6 }}>
            <button className="btn small" type="button" onClick={() => setLive({ find: id })}><Bluetooth size={18} />Bağlan</button>
            <button className="btn small" type="button" onClick={() => setLive({ usb: true })}><Usb size={18} />USB ile</button>
          </div>
        </>}
      </Card>

      {!isGateway && <>
        <Section title="Ayarlar" info="Ayar hesaba kaydedilir; tasmaya bağlanınca (şimdilik USB ile) sormadan yazılır. Tasma yazdığını onaylayınca 'Cihazda' olur. Birden çok tasmaya birden: Cihazlar → Toplu ayar." />
        <Card>
          {rows.map((r) => (
            <Cell key={r.def.key} title={r.def.title} wrapSub
              sub={rowSub(r)} subTone={r.state === "waiting" ? "aging" : undefined}
              trail={<span className="sm row" style={{ gap: 4 }}>{r.value !== null ? r.def.word(r.value) : "—"}<CaretRight size={16} color="var(--mute)" /></span>}
              onClick={() => setEditing(r)} />
          ))}
          <Cell title="Alanlar" wrapSub
            sub={fence ? (queuedFence.has(up) ? "Sırada · tasmaya bağlanınca işlenir" : stateLine(fence)) : "Bu tasmaya gidecek alan yok"}
            subTone={fence && (fence.state === "stale" || fence.state === "never") ? "aging" : undefined}
            trail={<CaretRight size={16} color="var(--mute)" />}
            onClick={() => (fence && (fence.state === "stale" || fence.state === "never") && !queuedFence.has(up) ? setSending(true) : nav.setTab("zones"))} />
          {fence?.record && fence.record.zones.length > 0 && (
            <KV k="Tasmadaki alanlar">{fence.record.zones.map((z) => z.name).join(", ")}</KV>
          )}
        </Card>
      </>}

      {!isGateway && myOps.length > 0 && <>
        <Section title="İşlemler" info="Her ayar kaydı ve 'Tasmalara uygula' bir işlemdir. Sıradakiler tasmaya bağlanınca (şimdilik USB) kendiliğinden işlenir; 'Tasmaya işlendi' yalnız tasma onaylayınca yazılır." />
        <Card>
          {myOps.map((t) => (
            <Cell key={t.op} title={KIND_WORD[t.kind]} wrapSub sub={<>{opSub(t)}<OpSteps t={t} /></>}
              subTone={t.state === "rejected" ? "alarm" : t.state === "applied" || t.state === "same" ? "fresh" : undefined} />
          ))}
        </Card>
      </>}

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

/** One setting for one device (bulk: Cihazlar → Toplu ayar). */
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
        <p className="fine" style={{ margin: 0 }}>Kaydedince hesaba yazılır; tasmaya bağlanınca (şimdilik USB ile) sormadan gider.</p>
        <div className="btns" style={{ padding: 0 }}>
          <button className="btn" onClick={onClose}>Vazgeç</button>
          <button className="btn primary" disabled={busy || value === null || value === row.value} onClick={() => { void save(); }}>Kaydet</button>
        </div>
        {error && <p className="sm t-aging" role="alert" style={{ margin: 0 }}>{error}</p>}
      </div>
    </Dialog>
  );
}
