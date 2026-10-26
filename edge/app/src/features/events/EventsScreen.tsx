// Olaylar: what needs a look now on top, then the whole book by day —
// warnings with measurement and arrival times, links, hand-offs, uploads and
// the owner's notes.
import { useState } from "react";
import {
  BatteryWarning, Bell, Bluetooth, CheckCircle, CloudArrowUp, CloudCheck, Cow, HardDrives, Key, Note, PersonSimpleWalk, WifiSlash,
} from "@phosphor-icons/react";
import { isActive } from "../../app/events.ts";
import { fmt, tr } from "../../app/i18n/tr.ts";
import type { AppEvent } from "../../app/model.ts";
import { useNav } from "../../app/nav.ts";
import { useApp } from "../../app/store.ts";
import { Card, Chips, DemoFlag, Empty, Header, Section } from "../../ds/components.tsx";

type Filter = "all" | "alerts" | "animals" | "system";

function iconOf(e: AppEvent) {
  switch (e.kind) {
    case "silent": case "lost": return <WifiSlash size={20} />;
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

export function EventRow({ e, now, seen, onSeen, onOpen }: {
  e: AppEvent; now: number; seen?: boolean; onSeen?: () => void; onOpen?: () => void;
}) {
  const active = isActive(e);
  const tone = toneOf(e);
  const meta: string[] = [];
  if (e.alert) {
    const gap = e.kind === "silent" || e.kind === "lost", still = e.kind === "still";
    if ((gap || still) && active) meta.push(gap ? tr.events.lastFix(fmt.clock(e.at)) : tr.events.started(fmt.clock(e.at)), tr.events.ongoing);
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
        {active && onSeen && (
          <span style={{ paddingTop: 6 }}>
            <button className="btn small" disabled={seen} onClick={(ev) => { ev.stopPropagation(); onSeen(); }}>
              {seen ? tr.events.seenDone : tr.events.seen}
            </button>
          </span>
        )}
      </span>
      {!e.alert && <span className="time">{fmt.clock(e.at)}</span>}
    </>
  );
  return onOpen ? <button className="ev" onClick={onOpen}>{body}</button> : <div className="ev">{body}</div>;
}

export function EventsScreen() {
  const m = useApp();
  const nav = useNav();
  const [filter, setFilter] = useState<Filter>("all");
  const active = m.events.filter(isActive);
  const rest = m.events.filter((e) => !isActive(e)).filter((e) =>
    filter === "all" || (filter === "alerts" && e.alert) || (filter === "animals" && e.asset !== null) ||
    (filter === "system" && e.asset === null));
  const groups: { label: string; items: AppEvent[] }[] = [];
  for (const e of rest.slice(0, 400)) {
    const label = fmt.day(e.at, m.now);
    if (!groups.length || groups[groups.length - 1].label !== label) groups.push({ label, items: [] });
    groups[groups.length - 1].items.push(e);
  }
  const open = (e: AppEvent) => (e.asset ? () => nav.push({ to: "animal", id: e.asset! }) : e.kind === "station-full" || e.kind === "server-down" ? () => nav.setTab("network") : undefined);

  return (
    <div className="page">
      <Header title={tr.events.title} />
      {m.demo && <DemoFlag onPage />}
      {!m.events.length ? (
        <Empty icon={<Bell size={34} />} title={tr.events.empty} body={tr.events.emptyBody} />
      ) : (
        <>
          <Section title={tr.events.now} />
          {active.length ? (
            <Card>{active.map((e) => <EventRow key={e.id} e={e} now={m.now} seen={m.seen.has(e.id)} onSeen={() => m.actions.markSeen(e.id)} onOpen={open(e)} />)}</Card>
          ) : <p className="sm lead">{tr.events.nothingNow}</p>}
          <Chips<Filter> value={filter} onChange={setFilter} options={[
            { value: "all", label: tr.events.filters.all }, { value: "alerts", label: tr.events.filters.alerts },
            { value: "animals", label: tr.events.filters.animals }, { value: "system", label: tr.events.filters.system },
          ]} />
          {groups.map((g) => (
            <div key={g.label}>
              <Section title={g.label} />
              <Card>{g.items.map((e) => <EventRow key={e.id} e={e} now={m.now} onOpen={open(e)} />)}</Card>
            </div>
          ))}
        </>
      )}
    </div>
  );
}
