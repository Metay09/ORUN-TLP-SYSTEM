// Olaylar (sahip isteği 2026-11-11: kullanıcı odaklı). Üstte tek cümle: her şey
// yolunda mı, kaç hayvana bakmalı. Sonra "Şimdi": hayvan başına bir kart, sorun,
// ne yapmalı ve o işin düğmesi (Bul, Haritada, Tasma, Bağlantı). Altta zaman
// çizelgesi: gün gün, saatli satırlar; Hayvanlar / Sistem süzgeci. Ham kayıt
// defteri en altta kapalı. Yeni olay üretmez: app/digest.ts'i gösterir.
import { useMemo, useRef, useState, type ReactNode, type TouchEvent } from "react";
import {
  BatteryWarning, Bell, Bluetooth, CheckCircle, CloudArrowUp, CloudCheck, Cow, HardDrives, Key, MapTrifold,
  NavigationArrow, Note, PersonSimpleWalk, Polygon, SealCheck, WifiSlash, CellSignalFull,
} from "@phosphor-icons/react";
import { digest, type Family, type NowItem } from "../../app/digest.ts";
import { isActive } from "../../app/events.ts";
import { cap, fmt, tr } from "../../app/i18n/tr.ts";
import type { AppEvent } from "../../app/model.ts";
import { useNav } from "../../app/nav.ts";
import { useApp } from "../../app/store.ts";
import { AnimalRing, Card, Chips, Empty, Header, Section } from "../../ds/components.tsx";
import type { Animal } from "../../app/model.ts";

function iconOf(e: AppEvent) {
  switch (e.kind) {
    case "silent": case "lost": return <WifiSlash size={20} />;
    case "out": return <Polygon size={20} />;
    case "battery-low": case "battery-critical": return <BatteryWarning size={20} />;
    case "still": return <PersonSimpleWalk size={20} />;
    case "station-full": return <HardDrives size={20} />;
    case "server-down": return <CloudArrowUp size={20} />;
    case "records": return <Cow size={20} />;
    case "handoff": return <CheckCircle size={20} />;
    case "upload": return <CloudCheck size={20} />;
    case "keys": return <Key size={20} />;
    case "note": return <Note size={20} />;
    default: return <Bluetooth size={20} />;
  }
}

const toneOf = (e: AppEvent) => (e.severity === "danger" ? "alarm" : e.severity === "warn" ? "aging" : e.kind === "note" ? "info" : e.kind === "handoff" || e.kind === "upload" ? "fresh" : "");

export function EventRow({ e, now, seen, onSeen, onRestore, onOpen, onFind, onMap }: {
  e: AppEvent; now: number; seen?: boolean; onSeen?: () => void; onRestore?: () => void;
  onOpen?: () => void; onFind?: () => void; onMap?: () => void;
}) {
  // Only deliberate horizontal swipes acknowledge an alert. Vertical scrolling
  // must keep working, and a swipe may not accidentally open the animal card.
  const touchStart = useRef<{ x: number; y: number } | null>(null);
  const suppressClickUntil = useRef(0);
  const swipeStart = (ev: TouchEvent<HTMLElement>) => {
    const t = ev.touches[0];
    touchStart.current = onSeen && !seen && t ? { x: t.clientX, y: t.clientY } : null;
  };
  const swipeEnd = (ev: TouchEvent<HTMLElement>) => {
    const origin = touchStart.current;
    touchStart.current = null;
    const t = ev.changedTouches[0];
    if (!origin || !t || !onSeen || seen) return;
    const dx = t.clientX - origin.x, dy = t.clientY - origin.y;
    if (Math.abs(dx) < 90 || Math.abs(dx) < 1.4 * Math.abs(dy)) return;
    suppressClickUntil.current = Date.now() + 650;
    onSeen(); // both left and right swipes = "Gördüm", never "Çözüldü"
  };
  const swipeProps = { onTouchStart: swipeStart, onTouchEnd: swipeEnd,
    onTouchCancel: () => { touchStart.current = null; } };
  const active = isActive(e);
  const tone = toneOf(e);
  const meta: string[] = [];
  if (e.alert) {
    const gap = e.kind === "silent" || e.kind === "lost", still = e.kind === "still";
    if (e.kind === "out") meta.push(active ? tr.events.left(fmt.clock(e.at)) : tr.events.span(fmt.clock(e.at), fmt.clock(e.end!)), active ? tr.events.ongoing : tr.events.backInside);
    else if ((gap || still) && active) meta.push(gap ? tr.events.lastFix(fmt.clock(e.at)) : tr.events.started(fmt.clock(e.at)), tr.events.ongoing);
    else if ((gap || still) && e.end) meta.push(tr.events.span(fmt.clock(e.at), fmt.clock(e.end)), gap ? tr.events.backAgain : tr.events.movedAgain);
    else {
      meta.push(tr.events.measured(fmt.clock(e.at)));
      if (e.arrivedAt && e.arrivedAt - e.at > 60_000) meta.push(tr.events.arrived(fmt.clock(e.arrivedAt)));
      meta.push(active ? tr.events.ongoing : e.end ? tr.events.resolved(fmt.ago(e.end, now)) : "");
    }
  }
  const body = (
    <>
      <span className={"ic " + (active || !e.alert ? tone : "")}>{iconOf(e)}</span>
      <span className="cm">
        <span className="ct" style={{ fontSize: 15 }}>{e.title}</span>
        {e.detail && <span className="cs wrap">{e.detail}</span>}
        {meta.length > 0 && <span className="meta">{meta.filter(Boolean).join(" · ")}</span>}
        {active && (onSeen || onRestore || onFind || onMap) && (
          <span className="ev-acts">
            {onFind && (
              <button className="btn small signal" onClick={(ev) => { ev.stopPropagation(); onFind(); }}>
                <NavigationArrow size={16} weight="fill" />{tr.animal.find}
              </button>
            )}
            {onMap && (
              <button className="btn small" onClick={(ev) => { ev.stopPropagation(); onMap(); }}>{tr.events.showOnMap}</button>
            )}
            {onSeen && (
              <button className="btn small quiet" onClick={(ev) => { ev.stopPropagation(); onSeen(); }}>
                {tr.events.seen}
              </button>
            )}
            {onRestore && (
              <button className="btn small quiet" onClick={(ev) => { ev.stopPropagation(); onRestore(); }}>
                Tekrar göster
              </button>
            )}
          </span>
        )}
      </span>
      {!e.alert && <span className="time">{fmt.clock(e.at)}</span>}
    </>
  );
  return onOpen
    ? <div className="ev" role="button" tabIndex={0} {...swipeProps}
        onClick={(ev) => {
          if (Date.now() < suppressClickUntil.current) { ev.preventDefault(); return; }
          onOpen();
        }}
        onKeyDown={(ev) => {
          if (ev.target !== ev.currentTarget) return;
          if (ev.key === "Enter" || ev.key === " ") { ev.preventDefault(); onOpen(); }
        }}>{body}</div>
    : <div className="ev" {...swipeProps}>{body}</div>;
}

type Act = { label: string; icon?: ReactNode; tone?: "signal" | "primary"; run: () => void };

/** The one or two things to DO about this item, by its heaviest reason. */
function actionsOf(i: NowItem, a: Animal | undefined, nav: ReturnType<typeof useNav>): Act[] {
  if (!a) return [{ label: "Bağlantı", icon: <CellSignalFull size={16} />, tone: "primary", run: () => nav.setTab("network") }];
  const find: Act = { label: tr.animal.find, icon: <NavigationArrow size={16} weight="fill" />, tone: "signal", run: () => nav.push({ to: "find", id: a.id }) };
  const map: Act = { label: "Haritada", icon: <MapTrifold size={16} />, run: () => nav.showOnMap(a.id) };
  const collar: Act = { label: "Tasma", icon: <CellSignalFull size={16} />, run: () => nav.push({ to: "device", id: a.device! }) };
  switch (i.kind) {
    case "lost": case "silent": case "out": return a.last ? [find, map] : [];
    case "still": return a.last ? [map] : [];
    case "battery-critical": case "battery-low": return a.device ? [collar] : [];
    default: return a.last ? [map] : [];
  }
}

function NowCard({ item, a, seen, onSeen, onRestore, onOpen, lead, acts }: {
  item: NowItem; a: Animal | undefined; seen: boolean; onSeen: () => void; onRestore: () => void; onOpen: () => void;
  lead: ReactNode; acts: Act[];
}) {
  const tone = item.severity === "danger" ? "alarm" : "aging";
  const [first, ...rest] = item.reasons;
  return (
    <div className={"now-card " + tone} role="button" tabIndex={0} onClick={onOpen}
      onKeyDown={(ev) => { if (ev.target === ev.currentTarget && (ev.key === "Enter" || ev.key === " ")) { ev.preventDefault(); onOpen(); } }}>
      <div className="row" style={{ gap: 12, alignItems: "flex-start" }}>
        {lead}
        <span className="cm">
          <span className="ct">{item.name}{a?.profile.tag ? <span className="cap" style={{ fontWeight: 400 }}> {a.profile.tag}</span> : null}</span>
          <span className={"now-reason t-" + tone}>{cap(first ?? "")}</span>
          {rest.length > 0 && <span className="cs wrap">ayrıca {rest.join(", ")}</span>}
          {item.advice && <span className="cs wrap">{item.advice}</span>}
        </span>
      </div>
      <div className="now-acts">
        {acts.map((x) => (
          <button key={x.label} className={"btn small" + (x.tone ? " " + x.tone : "")} onClick={(ev) => { ev.stopPropagation(); x.run(); }}>{x.icon}{x.label}</button>
        ))}
        <button className="btn small quiet" style={{ marginLeft: "auto" }} onClick={(ev) => { ev.stopPropagation(); (seen ? onRestore : onSeen)(); }}>
          {seen ? tr.events.digest.restore : tr.events.seen}
        </button>
      </div>
    </div>
  );
}

const FAMILY_ICON: Record<Family, ReactNode> = {
  gap: <WifiSlash size={16} />, still: <PersonSimpleWalk size={16} />, out: <Polygon size={16} />,
  battery: <BatteryWarning size={16} />, note: <Note size={16} />,
};

type Filter = "all" | "animals" | "system";

export function EventsScreen() {
  const m = useApp();
  const nav = useNav();
  const T = tr.events.digest;
  const [filter, setFilter] = useState<Filter>("all");
  const [days, setDays] = useState(3);
  const d = useMemo(() => digest(m.events, m.herd, m.now, 14), [m.events, m.herd, m.now]);
  const isSeen = (i: NowItem) => i.ids.every((id) => m.seen.has(id));
  const fresh = d.now.filter((i) => !isSeen(i)), acknowledged = d.now.filter(isSeen);
  const animal = (id: string | null) => (id ? m.herd.find((a) => a.id === id) : undefined);
  const openOf = (i: NowItem) => {
    const a = animal(i.asset);
    return a ? () => nav.push({ to: "animal", id: a.id }) : () => nav.setTab("network");
  };
  const leadOf = (i: NowItem) => {
    const a = animal(i.asset);
    return a ? <AnimalRing a={a} size={48} /> : <span className={"ic " + (i.severity === "danger" ? "alarm" : "aging")}><HardDrives size={22} /></span>;
  };
  const card = (i: NowItem, seen: boolean) => (
    <NowCard key={i.key} item={i} a={animal(i.asset)} seen={seen} lead={leadOf(i)} onOpen={openOf(i)}
      acts={actionsOf(i, animal(i.asset), nav)}
      onSeen={() => i.ids.forEach((id) => m.actions.markSeen(id))}
      onRestore={() => i.ids.forEach((id) => m.actions.unmarkSeen(id))} />
  );
  const grazeUntil = m.graze.forever ? tr.zones.graze.forever : m.graze.until ? tr.zones.graze.untilClock(fmt.clock(m.graze.until)) : "";
  const raw = m.events.slice(0, 200);

  // Herd at a glance: how many animals are fine right now.
  const hit = new Set(d.now.filter((i) => i.asset).map((i) => i.asset)).size;
  const ok = Math.max(0, m.herd.length - hit);
  const banner = fresh.length
    ? { tone: fresh.some((i) => i.severity === "danger") ? "alarm" : "aging", title: hit ? `${hit} hayvana bakmalısın` : "Bakılacak bir şey var",
        sub: ok ? `${ok} hayvan sorunsuz` : "" }
    : { tone: "fresh", title: "Her şey yolunda", sub: m.herd.length ? (acknowledged.length ? `${acknowledged.length} bildiğin sorun sürüyor` : `${m.herd.length} hayvanın hepsinden haber geliyor`) : "" };

  const shown = d.days.slice(0, days);
  const lines = (day: (typeof d.days)[number]) => [
    ...(filter !== "system" ? day.lines.map((l) => ({ kind: "animal" as const, at: l.at, l })) : []),
    ...(filter !== "animals" ? day.system.map((s) => ({ kind: "system" as const, at: s.last, s })) : []),
  ].sort((x, y) => y.at - x.at);

  return (
    <div className="page">
      <Header title={tr.events.title} />
      {!m.events.length && !m.herd.length ? (
        <Empty icon={<Bell size={34} />} title={tr.events.empty} body={tr.events.emptyBody} />
      ) : (
        <>
          <div className={"ev-banner " + banner.tone}>
            <span className="ic">{banner.tone === "fresh" ? <SealCheck size={24} weight="fill" /> : <Bell size={24} weight="fill" />}</span>
            <span className="cm">
              <span className="ct">{banner.title}</span>
              {banner.sub && <span className="cs">{banner.sub}</span>}
            </span>
          </div>
          {m.grazing && <p className="sm lead" style={{ paddingTop: 8 }}>{tr.events.grazingNote(grazeUntil)}</p>}

          {fresh.length > 0 && <>
            <Section title={`Şimdi · ${fresh.length}`} info="Hayvan başına tek kart: en önemli sorun, ne yapmalı ve o işin düğmesi. 'Gördüm' yalnız senin işaretin; sorun ancak yeni bir ölçüm gelince kapanır." />
            <div className="now-list">{fresh.map((i) => card(i, false))}</div>
          </>}
          {acknowledged.length > 0 && (
            <details className="tech">
              <summary>{T.seenStill(acknowledged.length)}</summary>
              <div className="now-list">{acknowledged.map((i) => card(i, true))}</div>
            </details>
          )}

          <Section title="Geçmiş" />
          <Chips<Filter> value={filter} onChange={setFilter} options={[
            { value: "all", label: "Hepsi" }, { value: "animals", label: "Hayvanlar" }, { value: "system", label: "Sistem" },
          ]} />
          {shown.map((day) => {
            const rows = lines(day);
            return (
              <div key={day.start}>
                <div className="tl-day"><b>{day.label}</b><span>{day.lines.length ? day.headline : filter === "system" ? "" : T.quiet}</span></div>
                {rows.length ? (
                  <Card>
                    {rows.map((r, k) => r.kind === "animal"
                      ? <TimelineRow key={k} at={r.l.at} tone={r.l.severity === "danger" ? "alarm" : r.l.severity === "warn" ? "aging" : "info"}
                          icon={FAMILY_ICON[r.l.family]} onClick={r.l.asset ? () => nav.push({ to: "animal", id: r.l.asset! }) : undefined}>
                          <strong>{r.l.name}</strong> {r.l.text}
                        </TimelineRow>
                      : <TimelineRow key={k} at={r.s.last} tone="" icon={<Bluetooth size={16} />}>{r.s.text}</TimelineRow>)}
                  </Card>
                ) : filter === "system" ? <p className="fine lead" style={{ margin: 0 }}>Bu gün sistem kaydı yok.</p> : null}
              </div>
            );
          })}
          {d.days.length > days && (
            <div style={{ padding: "8px 16px" }}>
              <button className="btn wide" onClick={() => setDays((n) => n + 7)}>Daha eski günler</button>
            </div>
          )}

          <details className="tech">
            <summary>Ham kayıt defteri · {m.events.length}</summary>
            <Card>{raw.map((e) => <EventRow key={e.id} e={e} now={m.now} />)}</Card>
          </details>
        </>
      )}
    </div>
  );
}

function TimelineRow({ at, tone, icon, children, onClick }: {
  at: number; tone: string; icon: ReactNode; children: ReactNode; onClick?: () => void;
}) {
  const body = (
    <>
      <span className="tl-time">{fmt.clock(at)}</span>
      <span className={"tl-ic " + tone}>{icon}</span>
      <span className="tl-text">{children}</span>
    </>
  );
  return onClick ? <button className="tl-row" onClick={onClick}>{body}</button> : <div className="tl-row">{body}</div>;
}

