// Hayvan profili: today (position, movement compared with the animal's own
// days, battery outlook, the collar), history by day and its events, and the
// identity / notebook / collar.
import { useMemo, useState, useSyncExternalStore } from "react";
import { Bluetooth, CaretRight, CellSignalFull, MapTrifold, NavigationArrow, NotePencil, PencilSimple, Play } from "@phosphor-icons/react";
import { activityBaseline, activityCoverage24h, batteryOutlook, busiestHour, dayStart, daySummaries, hourlyActivity, smoothBattery } from "../../app/activity.ts";
import { trackerSession } from "../../app/trackerSession.ts";
import { BatteryLife, useConfiguredInterval } from "../devices/BatteryLife.tsx";
import { epochMs, externallyPowered, readingPercent } from "../../app/herd.ts";
import { bleName } from "../../app/deviceNames.ts";
import { cap, fmt, tr } from "../../app/i18n/tr.ts";
import type { Animal, AppEvent } from "../../app/model.ts";
import { useBack, useNav } from "../../app/nav.ts";
import { useApp } from "../../app/store.ts";
import { AnimalRing, Badge, Card, Cell, Empty, KV, Screen, Section, Seg, Sparkline, Strip, toneOfStatus } from "../../ds/components.tsx";
import { EventRow } from "../events/EventsScreen.tsx";
import { stateLine, useTrackerFences } from "../zones/ApplyFence.tsx";
import { useDeviceSettings } from "../devices/DeviceScreen.tsx";
import { pendingKeys } from "../../app/deviceSettings.ts";
import { AddNote, EditAnimal } from "./dialogs.tsx";

type Part = "today" | "history" | "info";

function ageMonths(birth: string, now: number): number | null {
  const t = Date.parse(birth);
  if (!Number.isFinite(t)) return null;
  const b = new Date(t), n = new Date(now);
  return Math.max(0, (n.getFullYear() - b.getFullYear()) * 12 + n.getMonth() - b.getMonth());
}

export function AnimalScreen({ id }: { id: string }) {
  const m = useApp();
  const nav = useNav();
  const [part, setPart] = useState<Part>("today");
  const [dialog, setDialog] = useState<null | "edit" | "note">(null);
  useBack(() => nav.pop());
  const a = m.herd.find((x) => x.id === id) ?? null;
  if (!a) return <Screen title="" onBack={() => nav.pop()}><Empty icon={<NavigationArrow />} title={tr.herd.noMatch} /></Screen>;

  const age = ageMonths(a.profile.birth, m.now);
  const sub = [a.profile.tag, a.profile.breed, age !== null ? tr.animal.age(age) : ""].filter(Boolean).join(" · ");
  return (
    <Screen onBack={() => nav.pop()} actions={<button className="circ" onClick={() => setDialog("edit")} aria-label={tr.common.edit}><PencilSimple size={20} /></button>}>
      <div className="stack" style={{ alignItems: "center", textAlign: "center", padding: "4px 16px 8px" }}>
        <AnimalRing a={a} size={96} />
        <span className="disp" style={{ fontSize: 30 }}>{a.name}</span>
        {sub && <span className="sm">{sub}</span>}
        <Badge tone={toneOfStatus(a.status)}>{a.seen}</Badge>
      </div>
      <div className="btns">
        <button className="btn signal" disabled={!a.last} onClick={() => nav.push({ to: "find", id: a.id })}><NavigationArrow size={20} weight="fill" />{tr.animal.find}</button>
        <button className="btn" disabled={!a.last} onClick={() => nav.showOnMap(a.id)}><MapTrifold size={20} />{tr.animal.map}</button>
        <button className="btn" style={{ flex: "0 0 52px", padding: 0 }} onClick={() => setDialog("note")} aria-label={tr.animal.addNote}><NotePencil size={22} /></button>
      </div>
      <Seg<Part> value={part} onChange={setPart} options={[
        { value: "today", label: tr.animal.tabs.today }, { value: "history", label: tr.animal.tabs.history }, { value: "info", label: tr.animal.tabs.info },
      ]} />
      {part === "today" && <Today a={a} now={m.now} />}
      {part === "history" && <History a={a} now={m.now} events={m.events.filter((e) => e.asset === a.id && e.alert)} />}
      {part === "info" && <Info a={a} now={m.now} onEdit={() => setDialog("edit")} onNote={() => setDialog("note")} />}
      {dialog === "edit" && <EditAnimal a={a} onClose={() => setDialog(null)} />}
      {dialog === "note" && <AddNote a={a} onClose={() => setDialog(null)} />}
    </Screen>
  );
}

function Today({ a, now }: { a: Animal; now: number }) {
  const nav = useNav();
  const p = a.last;
  // Activity and battery come from every report, also those without a fix.
  const hours = useMemo(() => hourlyActivity(a.reports, now), [a.reports, now]);
  const activity = useMemo(() => activityCoverage24h(a.reports, now), [a.reports, now]);
  const base = useMemo(() => activityBaseline(a.reports, now), [a.reports, now]);
  const days = useMemo(() => daySummaries(a.points.filter((x) => epochMs(x.time) >= dayStart(now))), [a.points, now]);
  const today = days.find((d) => d.day === dayStart(now)) ?? null;
  const bat = a.batteryTrail;
  // Pil 2: FULL is 100 %; the trend and the days left come only from readings on the
  // battery itself (not on USB, not without a cell). Older firmware sends no power word.
  const toPct = (list: typeof bat) => smoothBattery(list.map((b) => ({ t: b.t, p: readingPercent(b) }))
    .filter((x): x is { t: number; p: number } => x.p !== null));
  const pct = useMemo(() => toPct(bat), [bat]);   // eslint-disable-line react-hooks/exhaustive-deps
  const outlook = useMemo(() => batteryOutlook(toPct(bat.filter((b) => !externallyPowered(b))), now), [bat, now]); // eslint-disable-line react-hooks/exhaustive-deps
  const collarSaysPower = bat.some((b) => b.power !== null && b.power !== undefined);
  const hasActivity = a.reports.some((x) => x.active_s !== null);
  const interval = useConfiguredInterval(a);
  const settings = useDeviceSettings();
  const session = useSyncExternalStore(trackerSession.subscribe, trackerSession.snapshot);
  if (!p) return <Empty icon={<NavigationArrow size={30} />} title={tr.status.waiting} />;
  const t = epochMs(p.time), r = p.received ? Date.parse(p.received) : null;
  const busy = busiestHour(hours);
  const hourAt = (i: number) => fmt.clock(now - 24 * 3600_000 + i * 3600_000);
  const total = activity.activeMs + activity.inactiveMs + activity.unknownMs || 1;
  const batTone = a.battery === "critical" ? "alarm" : a.battery === "low" ? "aging" : "fresh";
  const waiting = a.device ? pendingKeys(settings[a.device.toUpperCase()]).length : 0;
  const connected = !!a.device && session?.device === a.device.toUpperCase() && session.link.alive;

  return (
    <>
      <Section title={tr.animal.lastPosition} />
      <Card>
        <KV k={tr.animal.measured}>{fmt.clock(t)} · {fmt.ago(t, now)}</KV>
        {r !== null && p.delay_s !== null && p.delay_s > 60 && <KV k={tr.animal.arrived}>{fmt.clock(r)} · {tr.animal.delay(fmt.duration(p.delay_s * 1000))}</KV>}
        <KV k="Kalite">{cap(tr.animal.quality(p.sats, p.hdop))}</KV>
        <KV k="Konum">{fmt.coords(p.lat, p.lon)}</KV>
      </Card>

      <Section title="Hareket · son 24 saat"
        info="Tasmanın ivmeölçerinin bildirdiği hareket süresi. GPS mesafesi hareket sayılmaz; ölçülmeyen zaman hareketsiz kabul edilmez. Karşılaştırma, bu hayvanın en az 3 tam ölçülmüş önceki gününe göredir." />
      <Card pad>
        {hasActivity && activity.coveredMs > 0 ? (
          <div className="stack" style={{ gap: 12 }}>
            <div className="row" style={{ alignItems: "flex-end", justifyContent: "space-between", gap: 12 }}>
              <div className="stack" style={{ gap: 0 }}>
                <span className="disp" style={{ fontSize: 30 }}>{fmt.duration(activity.activeMs)}</span>
                <span className="sm">hareketli</span>
              </div>
              {base && <Badge tone={Math.abs(base.change) < 0.15 ? "fresh" : base.change < 0 ? "aging" : "info"}>
                {Math.abs(base.change) < 0.15 ? "Her zamanki gibi" : `Her zamankinden %${Math.round(Math.abs(base.change) * 100)} ${base.change < 0 ? "az" : "fazla"}`}
              </Badge>}
            </div>
            <div className="row" role="img"
              aria-label={`Son 24 saat: ${fmt.duration(activity.activeMs)} hareketli, ${fmt.duration(activity.inactiveMs)} durgun, ${fmt.duration(activity.unknownMs)} ölçüm yok.`}
              style={{ height: 12, borderRadius: 9, overflow: "hidden", gap: 2, alignItems: "stretch" }}>
              {activity.activeMs > 0 && <span style={{ flex: activity.activeMs / total, background: "var(--fresh)" }} />}
              {activity.inactiveMs > 0 && <span style={{ flex: activity.inactiveMs / total, background: "var(--silent)" }} />}
              {activity.unknownMs > 0 && <span style={{ flex: activity.unknownMs / total, background: "repeating-linear-gradient(45deg, var(--aging) 0 3px, transparent 3px 6px)" }} />}
            </div>
            <div className="legend" style={{ padding: 0, gap: 12 }}>
              <span><i style={{ background: "var(--fresh)" }} />Hareketli</span>
              <span><i style={{ background: "var(--silent)" }} />Durgun {fmt.duration(activity.inactiveMs)}</span>
              {activity.unknownMs > 0 && <span><i style={{ background: "var(--aging)" }} />Ölçüm yok {fmt.duration(activity.unknownMs)}</span>}
            </div>
            <Strip values={hours} tall />
            <div className="strip-axis"><span>{hourAt(0)}</span><span>{hourAt(12)}</span><span>şimdi</span></div>
            <div className="act-facts">
              {busy !== null && <div><span className="cap">En hareketli saat</span><b>{hourAt(busy)}–{hourAt(busy + 1)}</b></div>}
              {today && <div><span className="cap">Bugün yürüdüğü</span><b>~{fmt.distance(today.walkedM)}</b></div>}
            </div>
          </div>
        ) : (
          <div className="stack">
            <span className="sm">Bu tasmadan son 24 saat için hareket ölçümü gelmedi.</span>
            {today && <span className="strong">{tr.animal.walkedToday(fmt.distance(today.walkedM))}</span>}
          </div>
        )}
      </Card>

      <Section title={tr.animal.battery} info="Yüzde, pil geriliminden hesaplanır (4,19 V %100, 4,05 V %90, 3,40 V %0). USB'de şarjı biten pil 'Dolu' ve %100 görünür. 'Şarj oluyor', 'Dolu' ve 'Pil takılı değil' tasmanın kendi ölçümünden gelir. Gidişat, son 24 saatin ortalaması iki gün öncekiyle karşılaştırılarak bulunur (gece-gündüz farkı ortalanır); USB'deki süreler katılmaz. Kalan gün yalnız belirgin düşüşte gösterilir ve tahmindir." />
      <Card pad>
        {a.batteryPower === "NO_BATTERY" ? <span className="sm">Pil takılı değil · USB'den besleniyor.</span>
          : a.battery === "unknown" ? <span className="sm">{tr.animal.batteryUnknown}</span> : (
          <div className="stack" style={{ gap: 12 }}>
            <div className="row" style={{ gap: 14 }}>
              <BatteryGauge percent={a.batteryPercent} tone={batTone} />
              <div className="stack grow" style={{ gap: 2 }}>
                <span className={"strong t-" + batTone}>
                  {a.batteryPower === "CHARGING" ? "Şarj oluyor" : a.batteryPower === "FULL" ? "Dolu"
                    : a.battery === "critical" ? "Bitmek üzere · bugün şarj et" : a.battery === "low" ? "Azalıyor" : "İyi"}
                </span>
                <span className="sm">
                  {a.batteryUsb ? "USB'de · " : ""}{a.batteryMv ? `${fmt.dec(a.batteryMv / 1000, 2)} V · ` : ""}{a.batteryAt !== null ? `${fmt.ago(a.batteryAt, now)} ölçüldü` : ""}
                </span>
              </div>
            </div>
            <div className="act-facts">
              {outlook.change24h !== null && <div><span className="cap">Son 24 saatte</span><b>{outlook.change24h >= 0 ? "+" : "−"}%{Math.abs(Math.round(outlook.change24h))}</b></div>}
              {!collarSaysPower && outlook.charged24h !== null && outlook.charged24h >= 3 && <div><span className="cap">Gündüz şarj</span><b>+%{Math.round(outlook.charged24h)}</b></div>}
              {outlook.perDay !== null && <div><span className="cap">Gidişat</span><b>{outlook.perDay < -0.5 ? `günde −%${fmt.dec(-outlook.perDay, 1)}` : "Dengede"}</b></div>}
            </div>
            {pct.length > 1 && <>
              <Sparkline values={pct.map((b) => ({ t: b.t, v: b.p }))} min={0} max={100} tone={batTone} />
              <div className="strip-axis"><span>{fmt.dateShort(pct[0].t)}</span><span>{tr.animal.batteryTrend}</span><span>şimdi</span></div>
            </>}
          </div>
        )}
      </Card>

      {a.device && a.batteryPower !== "NO_BATTERY" && <BatteryLife device={a.device} intervalS={interval} series={a.batteryTrail} nowPct={a.batteryPercent} />}

      {a.device && <>
        <Section title={tr.animal.device} />
        <Card>
          <Cell lead={<span className={"ic " + (connected ? "fresh" : "brand")}>{connected ? <Bluetooth size={20} /> : <CellSignalFull size={20} />}</span>}
            title={connected ? "Bluetooth ile bağlı" : bleName(a.device)} wrapSub
            sub={[interval ? `Rapor aralığı ${fmt.period(interval)}` : "", waiting ? `${waiting} ayar sırada` : "Ayarlar, bağlan"].filter(Boolean).join(" · ")}
            subTone={waiting ? "aging" : undefined}
            trail={<CaretRight size={18} color="var(--mute)" />}
            onClick={() => nav.push({ to: "device", id: a.device! })} />
        </Card>
      </>}
    </>
  );
}

/** Battery body with fill and the percentage. */
function BatteryGauge({ percent, tone }: { percent: number | null; tone: string }) {
  const v = percent ?? 0;
  return (
    <span className={"batt " + tone} aria-label={percent !== null ? `Pil %${percent}` : "Pil"}>
      <span className="batt-body"><i style={{ width: `${Math.max(4, Math.min(100, v))}%` }} /></span>
      <span className="batt-cap" />
      <b>{percent !== null ? `%${percent}` : "—"}</b>
    </span>
  );
}

function History({ a, now, events }: { a: Animal; now: number; events: AppEvent[] }) {
  const nav = useNav();
  const days = useMemo(() => daySummaries(a.points), [a.points]);
  return (
    <>
      <Section title={tr.animal.days} />
      {days.length ? (
        <Card>
          {days.slice(0, 30).map((d) => (
            <Cell key={d.day} title={fmt.day(d.day, now)}
              sub={tr.animal.dayLine(d.records, fmt.distance(d.walkedM), d.activeMs !== null ? fmt.duration(d.activeMs) : null)}
              lead={<span className="ic brand"><Play size={18} weight="fill" /></span>}
              trail={<CaretRight size={18} color="var(--mute)" />}
              onClick={() => nav.showOnMap(a.id, { from: d.day, to: d.day + 24 * 3600_000 - 1 })} />
          ))}
        </Card>
      ) : <p className="sm lead">{tr.animal.noHistory}</p>}
      <p className="fine" style={{ padding: "0 22px" }}>{tr.animal.dayOnMap}</p>
      <Section title={tr.animal.events} />
      {events.length ? <Card>{events.slice(0, 40).map((e) => <EventRow key={e.id} e={e} now={now} />)}</Card>
        : <p className="sm lead">{tr.animal.noEvents}</p>}
    </>
  );
}

function Info({ a, now, onEdit, onNote }: { a: Animal; now: number; onEdit: () => void; onNote: () => void }) {
  const f = tr.animal.fields, p = a.profile;
  const age = ageMonths(p.birth, now);
  const dev = useApp().devices.find((d) => d.id === a.device) ?? null;
  const fence = useTrackerFences().find((x) => a.device && x.device.toUpperCase() === a.device.toUpperCase()) ?? null;
  const nav = useNav();
  const settings = useDeviceSettings();
  const waiting = a.device ? pendingKeys(settings[a.device.toUpperCase()]).length : 0;
  const interval = useConfiguredInterval(a);
  return (
    <>
      <Section title={tr.animal.identity} action={tr.common.edit} onAction={onEdit} />
      <Card>
        <KV k={f.name}>{a.named ? a.name : "—"}</KV>
        <KV k={f.tag}>{p.tag || "—"}</KV>
        <KV k={f.breed}>{p.breed || "—"}</KV>
        <KV k={f.sex}>{tr.animal.sex[p.sex]}</KV>
        <KV k={f.birth}>{p.birth ? `${fmt.dateShort(Date.parse(p.birth))} ${new Date(p.birth).getFullYear()}${age !== null ? ` · ${tr.animal.age(age)}` : ""}` : "—"}</KV>
      </Card>

      <Section title={tr.animal.notebook} action={tr.animal.addNote} onAction={onNote} />
      <Card>
        {p.notes.length ? [...p.notes].reverse().map((n) => (
          <div key={n.at} className="ev"><span className="grow">{n.text}</span><span className="time">{fmt.dateTime(n.at)}</span></div>
        )) : <p className="sm" style={{ padding: "14px 16px", margin: 0 }}>{tr.animal.notebookEmpty}</p>}
      </Card>

      <Section title={tr.animal.device} />
      <Card>
        {a.device ? (
          <>
            <KV k="Bluetooth adı">{bleName(a.device)}</KV>
            <KV k={tr.device.lastContact}>{dev?.lastTime ? `${fmt.clock(dev.lastTime)} · ${fmt.ago(dev.lastTime, now)}` : "—"}</KV>
            <KV k={tr.animal.battery}>{dev && dev.batteryPercent !== null ? `%${dev.batteryPercent}${dev.batteryMv ? ` · ${fmt.dec(dev.batteryMv / 1000, 2)} V` : ""}` : "—"}</KV>
            <KV k={tr.device.report}>{interval ? fmt.period(interval) : "—"}</KV>
            {fence && <KV k="Alanlar">{stateLine(fence)}</KV>}
            <Cell title="Tasma ekranı" sub={waiting ? `${waiting} ayar sırada · bağlan, ayarlar` : "Bağlan, ayarlar, alanlar"}
              subTone={waiting ? "aging" : undefined} trail={<CaretRight size={18} color="var(--mute)" />}
              onClick={() => nav.push({ to: "device", id: a.device! })} />
          </>
        ) : <p className="sm" style={{ padding: "14px 16px", margin: 0 }}>{tr.animal.noDevice}</p>}
      </Card>
      {a.device && (
        <details className="tech">
          <summary>{tr.common.tech}</summary>
          <Card><KV k="Kimlik"><code>{a.device}</code></KV></Card>
        </details>
      )}
    </>
  );
}
