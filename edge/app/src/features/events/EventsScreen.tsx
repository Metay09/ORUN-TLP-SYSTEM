// Olaylar: işlenmiş özet (app/digest.ts). Üstte hayvan başına tek kartla şimdi
// bakılacaklar, sonra gün gün birleştirilmiş olaylar ve sistem satırları; ham
// kayıtların tamamı en altta kapalı durur.
import { useMemo, useRef, type ReactNode, type TouchEvent } from "react";
import {
  BatteryWarning, Bell, Bluetooth, CheckCircle, CloudArrowUp, CloudCheck, Cow, HardDrives, Key, NavigationArrow, Note,
  PersonSimpleWalk, Polygon, WifiSlash,
} from "@phosphor-icons/react";
import { digest, type NowItem } from "../../app/digest.ts";
import { isActive } from "../../app/events.ts";
import { fmt, tr } from "../../app/i18n/tr.ts";
import type { AppEvent } from "../../app/model.ts";
import { useNav } from "../../app/nav.ts";
import { useApp } from "../../app/store.ts";
import { AnimalRing, Card, Empty, Header, Section } from "../../ds/components.tsx";

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

function NowCard({ item, seen, onSeen, onRestore, onOpen, lead }: {
  item: NowItem; seen: boolean; onSeen: () => void; onRestore: () => void; onOpen?: () => void; lead: ReactNode;
}) {
  const tone = item.severity === "danger" ? "t-alarm" : "t-aging";
  const open = () => onOpen?.();
  return (
    <div className="ev" role={onOpen ? "button" : undefined} tabIndex={onOpen ? 0 : undefined} onClick={open}
      onKeyDown={(ev) => { if (ev.target === ev.currentTarget && (ev.key === "Enter" || ev.key === " ")) { ev.preventDefault(); open(); } }}>
      {lead}
      <span className="cm">
        <span className="ct" style={{ fontSize: 16 }}>{item.name}</span>
        <span className={"cs wrap strong " + tone}>{item.reasons.join(" · ")}</span>
        {item.advice && <span className="cs wrap">{item.advice}</span>}
        <span className="ev-acts">
          <button className="btn small quiet" onClick={(ev) => { ev.stopPropagation(); (seen ? onRestore : onSeen)(); }}>
            {seen ? tr.events.digest.restore : tr.events.seen}
          </button>
        </span>
      </span>
    </div>
  );
}

export function EventsScreen() {
  const m = useApp();
  const nav = useNav();
  const T = tr.events.digest;
  const d = useMemo(() => digest(m.events, m.herd, m.now), [m.events, m.herd, m.now]);
  const isSeen = (i: NowItem) => i.ids.every((id) => m.seen.has(id));
  const fresh = d.now.filter((i) => !isSeen(i)), acknowledged = d.now.filter(isSeen);
  const animal = (id: string | null) => (id ? m.herd.find((a) => a.id === id) : undefined);
  // A located animal opens on the map (Bul is there); otherwise its card; system items open Bağlantı.
  const openOf = (i: NowItem) => {
    const a = animal(i.asset);
    if (a) return a.last ? () => nav.showOnMap(a.id) : () => nav.push({ to: "animal", id: a.id });
    return () => nav.setTab("network");
  };
  const leadOf = (i: NowItem) => {
    const a = animal(i.asset);
    return a ? <AnimalRing a={a} size={44} /> : <span className={"ic " + (i.severity === "danger" ? "alarm" : "aging")}><HardDrives size={20} /></span>;
  };
  const card = (i: NowItem, seen: boolean) => (
    <NowCard key={i.key} item={i} seen={seen} lead={leadOf(i)} onOpen={openOf(i)}
      onSeen={() => i.ids.forEach((id) => m.actions.markSeen(id))}
      onRestore={() => i.ids.forEach((id) => m.actions.unmarkSeen(id))} />
  );
  const grazeUntil = m.graze.forever ? tr.zones.graze.forever : m.graze.until ? tr.zones.graze.untilClock(fmt.clock(m.graze.until)) : "";
  const raw = m.events.slice(0, 200);

  return (
    <div className="page">
      <Header title={tr.events.title} />
      {!m.events.length ? (
        <Empty icon={<Bell size={34} />} title={tr.events.empty} body={tr.events.emptyBody} />
      ) : (
        <>
          <Section title={T.now(fresh.length)} />
          {m.grazing && <p className="sm lead">{tr.events.grazingNote(grazeUntil)}</p>}
          {fresh.length ? <Card>{fresh.map((i) => card(i, false))}</Card> : <p className="sm lead">{T.allGood}</p>}
          {acknowledged.length > 0 && (
            <details className="tech">
              <summary>{T.seenStill(acknowledged.length)}</summary>
              <Card>{acknowledged.map((i) => card(i, true))}</Card>
            </details>
          )}

          <Section title={T.days} />
          {d.days.map((day) => (
            <Card key={day.start} pad>
              <div className="row"><strong className="grow">{day.label}</strong><span className="sm">{day.headline}</span></div>
              {day.lines.map((l, k) => (
                <button key={k} className="digest-line" onClick={() => l.asset && nav.push({ to: "animal", id: l.asset })}>
                  <span className={"dot " + (l.severity === "danger" ? "alarm" : l.severity === "warn" ? "aging" : "")} />
                  <span><strong>{l.name}</strong> {l.text}</span>
                </button>
              ))}
              {day.system.length > 0 && (
                <details className="tech">
                  <summary>{T.system(day.system.length)}</summary>
                  {day.system.map((s, k) => <p key={k} className="sm" style={{ margin: "6px 0 0" }}>{fmt.clock(s.last)} · {s.text}</p>)}
                </details>
              )}
            </Card>
          ))}

          <details className="tech">
            <summary>{T.raw(m.events.length)}</summary>
            <Card>{raw.map((e) => <EventRow key={e.id} e={e} now={m.now} />)}</Card>
          </details>
        </>
      )}
    </div>
  );
}
