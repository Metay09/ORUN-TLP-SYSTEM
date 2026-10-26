// Sürü: every animal, what needs a look first; search, filters, today's
// herd summary from the records.
import { useMemo, useState } from "react";
import { CaretRight, Cow, MagnifyingGlass, Plus } from "@phosphor-icons/react";
import { dayStart, daySummaries, hourlyActivity } from "../../app/activity.ts";
import { herdSummary, matches, needsLook } from "../../app/herd.ts";
import { fmt, tr } from "../../app/i18n/tr.ts";
import type { Animal } from "../../app/model.ts";
import { useNav } from "../../app/nav.ts";
import { useApp } from "../../app/store.ts";
import { AnimalRing, Card, Chips, DemoFlag, Empty, Header, Strip, toneOfStatus } from "../../ds/components.tsx";
import { AddDevice } from "./dialogs.tsx";

type Filter = "all" | "attention" | "silent" | "battery";

export function HerdScreen() {
  const m = useApp();
  const nav = useNav();
  const [q, setQ] = useState("");
  const [filter, setFilter] = useState<Filter>("all");
  const [adding, setAdding] = useState(false);
  const herd = m.herd;

  const counts = {
    attention: herd.filter(needsLook).length,
    silent: herd.filter((a) => a.status === "silent" || a.status === "lost").length,
    battery: herd.filter((a) => a.battery === "low" || a.battery === "critical").length,
  };
  const list = herd.filter((a) => matches(a, q) && (
    filter === "all" || (filter === "attention" && needsLook(a)) ||
    (filter === "silent" && (a.status === "silent" || a.status === "lost")) ||
    (filter === "battery" && (a.battery === "low" || a.battery === "critical"))));

  const today = useMemo(() => todaySummary(herd, m.now), [herd, m.now]);

  return (
    <div className="page">
      <Header title={tr.herd.title}>
        <button className="circ brand" onClick={() => setAdding(true)} aria-label={tr.herd.add}><Plus size={22} weight="bold" /></button>
      </Header>
      {m.demo && <DemoFlag onPage />}
      {herd.length === 0 ? (
        <Empty icon={<Cow size={34} />} title={tr.states.noAnimals} body={tr.states.noAnimalsBody}>
          <button className="btn primary" onClick={() => nav.setTab("network")}>{tr.states.connect}</button>
          {m.canLeaveDemo && !m.demo && <button className="btn quiet" onClick={() => m.actions.setPrefs({ demo: true })}>{tr.states.tryDemo}</button>}
        </Empty>
      ) : (
        <>
          <p className="sm lead" style={{ margin: 0 }}>{herdSummary(herd)}</p>
          <div className="search">
            <MagnifyingGlass size={20} />
            <input className="inp" type="search" value={q} placeholder={tr.herd.search} aria-label={tr.herd.search} onChange={(e) => setQ(e.target.value)} />
          </div>
          {today && (
            <Card pad hero>
              <div className="row" style={{ justifyContent: "space-between", alignItems: "baseline" }}>
                <span className="cap strong">{tr.herd.today}</span>
                <span className="cap">{fmt.clock(today.first)} – {fmt.clock(today.last)}</span>
              </div>
              <div className="row" style={{ gap: 22, paddingTop: 12, alignItems: "flex-start" }}>
                {today.activeMs !== null && (
                  <div className="stack" style={{ gap: 0 }}><span className="disp" style={{ fontSize: 28 }}>{fmt.duration(today.activeMs)}</span><span className="cap">{tr.herd.avgActive}</span></div>
                )}
                <div className="stack" style={{ gap: 0 }}><span className="disp" style={{ fontSize: 28 }}>{fmt.distance(today.walkedM)}</span><span className="cap">{tr.herd.avgWalk}</span></div>
              </div>
              <div className="cap" style={{ paddingTop: 10 }}>{tr.herd.located(today.animals)}</div>
            </Card>
          )}
          <Chips<Filter> value={filter} onChange={setFilter} options={[
            { value: "all", label: tr.herd.filters.all },
            { value: "attention", label: tr.herd.filters.attention, n: counts.attention },
            { value: "silent", label: tr.herd.filters.silent, n: counts.silent },
            { value: "battery", label: tr.herd.filters.battery, n: counts.battery },
          ]} />
          {list.length ? (
            <Card>
              {list.map((a) => <HerdCell key={a.id} a={a} now={m.now} onOpen={() => nav.push({ to: "animal", id: a.id })} />)}
            </Card>
          ) : <p className="sm" style={{ padding: "16px 20px" }}>{tr.herd.noMatch}</p>}
          <p className="cap" style={{ padding: "4px 22px 8px" }}>{tr.herd.ringHelp}</p>
        </>
      )}
      {adding && <AddDevice onClose={() => setAdding(false)} />}
    </div>
  );
}

function HerdCell({ a, now, onOpen }: { a: Animal; now: number; onOpen: () => void }) {
  const hasActivity = a.points.some((p) => p.active_s !== null);
  return (
    <button className="cell" onClick={onOpen}>
      <AnimalRing a={a} />
      <span className="cm">
        <span className="ct">{a.name}{a.profile.tag && <span className="cap" style={{ fontWeight: 400 }}> {a.profile.tag}</span>}</span>
        <span className={`cs strong t-${toneOfStatus(a.status)}`}>{a.seen}</span>
        {hasActivity && <span style={{ marginTop: 4 }}><Strip values={hourlyActivity(a.points, now)} /></span>}
      </span>
      <span className="stack" style={{ alignItems: "flex-end", gap: 4 }}>
        {a.batteryPercent !== null && <span className={"cap" + (a.battery === "critical" ? " t-alarm strong" : a.battery === "low" ? " t-aging strong" : "")}>%{a.batteryPercent}</span>}
        <CaretRight size={18} color="var(--mute)" />
      </span>
    </button>
  );
}

function todaySummary(herd: Animal[], now: number) {
  const start = dayStart(now);
  let animals = 0, walked = 0, active = 0, withActive = 0, first = Infinity, last = -Infinity;
  for (const a of herd) {
    const d = daySummaries(a.points.filter((p) => Date.parse(p.time ?? "") >= start)).find((x) => x.day === start);
    if (!d) continue;
    animals++;
    walked += d.walkedM;
    if (d.activeMs !== null) { active += d.activeMs; withActive++; }
    first = Math.min(first, d.first);
    last = Math.max(last, d.last);
  }
  if (!animals) return null;
  return { animals, walkedM: walked / animals, activeMs: withActive ? active / withActive : null, first, last };
}
