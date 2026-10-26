// Hayvan profili: today (position, movement, distance, battery), history by
// day and its events, and the identity / notebook / collar.
import { useMemo, useState } from "react";
import { CaretRight, MapTrifold, NavigationArrow, NotePencil, PencilSimple, Play } from "@phosphor-icons/react";
import { batterySeries, dayStart, daySummaries, hourlyActivity } from "../../app/activity.ts";
import { batteryPercent, epochMs, shortId } from "../../app/herd.ts";
import { cap, fmt, tr } from "../../app/i18n/tr.ts";
import type { Animal, AppEvent } from "../../app/model.ts";
import { useBack, useNav } from "../../app/nav.ts";
import { useApp } from "../../app/store.ts";
import { AnimalRing, Badge, Card, Cell, Empty, KV, Screen, Section, Seg, Sparkline, Strip, toneOfStatus } from "../../ds/components.tsx";
import { EventRow } from "../events/EventsScreen.tsx";
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
  const p = a.last;
  const hours = useMemo(() => hourlyActivity(a.points, now), [a.points, now]);
  const days = useMemo(() => daySummaries(a.points.filter((x) => epochMs(x.time) >= dayStart(now))), [a.points, now]);
  const today = days.find((d) => d.day === dayStart(now)) ?? null;
  const bat = useMemo(() => batterySeries(a.points, now), [a.points, now]);
  const hasActivity = a.points.some((x) => x.active_s !== null);
  if (!p) return <Empty icon={<NavigationArrow size={30} />} title={tr.status.waiting} />;
  const t = epochMs(p.time), r = p.received ? Date.parse(p.received) : null;
  return (
    <>
      <Section title={tr.animal.lastPosition} />
      <Card>
        <KV k={tr.animal.measured}>{fmt.clock(t)} · {fmt.ago(t, now)}</KV>
        {r !== null && <KV k={tr.animal.arrived}>{fmt.clock(r)}{p.delay_s !== null && p.delay_s > 60 ? ` · ${tr.animal.delay(fmt.duration(p.delay_s * 1000))}` : ""}</KV>}
        <KV k="Kalite">{cap(tr.animal.quality(p.sats, p.hdop))}</KV>
        <KV k="Konum">{fmt.coords(p.lat, p.lon)}</KV>
      </Card>

      <Section title={tr.animal.activity} />
      <Card pad>
        {hasActivity ? (
          <div className="stack">
            <span className="strong">{today?.activeMs !== null && today?.activeMs !== undefined ? tr.animal.activityToday(fmt.duration(today.activeMs)) : tr.animal.activity}</span>
            <Strip values={hours} tall />
            <div className="strip-axis"><span>{fmt.clock(now - 24 * 3600_000)}</span><span>{fmt.clock(now - 12 * 3600_000)}</span><span>{fmt.clock(now)}</span></div>
            <span className="fine">{tr.animal.activityStripHelp}</span>
          </div>
        ) : <span className="sm">{tr.animal.activityNone}</span>}
      </Card>

      {today && (
        <>
          <Section title={tr.animal.walked} />
          <Card pad>
            <div className="stack" style={{ gap: 4 }}>
              <span className="disp" style={{ fontSize: 26 }}>{tr.animal.walkedToday(fmt.distance(today.walkedM))}</span>
              <span className="fine">{tr.animal.walkedHelp}</span>
            </div>
          </Card>
        </>
      )}

      <Section title={tr.animal.battery} />
      <Card pad>
        {a.battery === "unknown" ? <span className="sm">{tr.animal.batteryUnknown}</span> : (
          <div className="stack">
            <span className={"strong" + (a.battery === "critical" ? " t-alarm" : a.battery === "low" ? " t-aging" : "")}>
              {a.battery === "critical" ? tr.animal.batteryCritical : a.battery === "low" ? tr.animal.batteryLow(a.batteryPercent) : tr.animal.batteryGood(a.batteryPercent)}
            </span>
            {bat.length > 1 && <>
              <Sparkline values={bat.map((b) => ({ t: b.t, v: batteryPercent(b.mv) ?? 0 }))} min={0} max={100}
                tone={a.battery === "critical" ? "alarm" : a.battery === "low" ? "aging" : "fresh"} />
              <span className="fine">{tr.animal.batteryTrend}</span>
            </>}
          </div>
        )}
      </Card>
    </>
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
  const nav = useNav();
  const f = tr.animal.fields, p = a.profile;
  const age = ageMonths(p.birth, now);
  const dev = useApp().devices.find((d) => d.id === a.device) ?? null;
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
          <Cell title={tr.animal.deviceLine(shortId(a.device))}
            sub={[a.periodS ? tr.animal.reportEvery(fmt.period(a.periodS)) : "", dev ? tr.animal.records(dev.records) : ""].filter(Boolean).join(" · ")}
            trail={<CaretRight size={18} color="var(--mute)" />} onClick={() => nav.push({ to: "device", id: a.device! })} />
        ) : <p className="sm" style={{ padding: "14px 16px", margin: 0 }}>{tr.animal.noDevice}</p>}
      </Card>
    </>
  );
}
