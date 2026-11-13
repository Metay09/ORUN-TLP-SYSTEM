// Sürü: every animal, what needs a look first; search and filters. (The herd
// "today" averages were dropped 2026-11-11: the animal card says it better.)
import { useState } from "react";
import { CaretRight, Cow, MagnifyingGlass, Plus } from "@phosphor-icons/react";
import { hourlyActivity } from "../../app/activity.ts";
import { herdSummary, matches, needsLook } from "../../app/herd.ts";
import { tr } from "../../app/i18n/tr.ts";
import type { Animal } from "../../app/model.ts";
import { useNav } from "../../app/nav.ts";
import { useApp } from "../../app/store.ts";
import { AnimalRing, Card, Chips, Empty, Header, Strip, toneOfStatus } from "../../ds/components.tsx";
import { AddTrackerDialog } from "./AddTrackerDialog.tsx";

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


  return (
    <div className="page">
      <Header title={tr.herd.title}>
        <button className="circ brand" onClick={() => setAdding(true)} aria-label={tr.herd.add}><Plus size={22} weight="bold" /></button>
      </Header>
      {herd.length === 0 && m.starting ? (
        <Empty icon={<Cow size={34} />} title={tr.states.starting} body="" />
      ) : herd.length === 0 ? (
        <Empty icon={<Cow size={34} />} title={tr.states.noAnimals} body={tr.states.noAnimalsBody}>
          <button className="btn primary" onClick={() => setAdding(true)}><Plus size={18} weight="bold" />{tr.herd.add}</button>
          <button className="btn" onClick={() => nav.setTab("network")}>{tr.states.connect}</button>
        </Empty>
      ) : (
        <>
          <p className="sm lead" style={{ margin: 0 }}>{herdSummary(herd)}</p>
          <div className="search">
            <MagnifyingGlass size={20} />
            <input className="inp" type="search" value={q} placeholder={tr.herd.search} aria-label={tr.herd.search} onChange={(e) => setQ(e.target.value)} />
          </div>
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
      {adding && <AddTrackerDialog onClose={() => setAdding(false)} />}
    </div>
  );
}

function HerdCell({ a, now, onOpen }: { a: Animal; now: number; onOpen: () => void }) {
  const hasActivity = a.reports.some((p) => p.active_s !== null);
  return (
    <button className="cell" onClick={onOpen}>
      <AnimalRing a={a} />
      <span className="cm">
        <span className="ct">{a.name}{a.profile.tag && <span className="cap" style={{ fontWeight: 400 }}> {a.profile.tag}</span>}</span>
        <span className={`cs strong t-${toneOfStatus(a.status)}`}>{a.seen}</span>
        {hasActivity && <span style={{ marginTop: 4 }}><Strip values={hourlyActivity(a.reports, now)} /></span>}
      </span>
      <span className="stack" style={{ alignItems: "flex-end", gap: 4 }}>
        {a.batteryPercent !== null && <span className={"cap" + (a.battery === "critical" ? " t-alarm strong" : a.battery === "low" ? " t-aging strong" : "")}>%{a.batteryPercent}</span>}
        <CaretRight size={18} color="var(--mute)" />
      </span>
    </button>
  );
}
