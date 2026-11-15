// The period a map trail shows (owner decision 2026-11-16): Son 2 saat ·
// Bugün · Dün · a calendar day, then ‹ day › and an hour range. Used for the
// selected animal's trail and, with no animal selected, the herd layers.
import { CalendarBlank, CaretLeft, CaretRight } from "@phosphor-icons/react";
import { HISTORY_DAYS } from "../../app/events.ts";
import { fmt } from "../../app/i18n/tr.ts";
import {
  addDays, fromIsoDate, isoDate, setHours, startOfDay, wholeDay, type TrailPick,
} from "./trailWindow.ts";

const RECENT: TrailPick = { kind: "recent" };
const HOURS = Array.from({ length: 25 }, (_, h) => h);
const hh = (h: number) => `${String(h).padStart(2, "0")}:00`;

/** Short label of a pick, e.g. "Son 2 saat", "Bugün", "Dün 06:00–09:00". */
export function pickLabel(p: TrailPick, now: number): string {
  if (p.kind === "recent") return "Son 2 saat";
  const day = fmt.day(p.day, now);
  return p.fromH === 0 && p.toH === 24 ? day : `${day} ${hh(p.fromH)}–${hh(p.toH)}`;
}

/** Son 2 saat · Bugün · Dün · a calendar day; then ‹ day › and the hours. */
export function TrailPicker({ pick, now, onPick }: { pick: TrailPick; now: number; onPick: (p: TrailPick) => void }) {
  const today = startOfDay(now), yday = addDays(today, -1), first = addDays(today, -(HISTORY_DAYS - 1));
  const which = pick.kind === "recent" ? "recent" : pick.day === today ? "today" : pick.day === yday ? "yday" : "other";
  const chip = (on: boolean, label: string, p: TrailPick) => (
    <button className="chip" aria-pressed={on} onClick={() => onPick(p)}>{label}</button>
  );
  return (
    <div className="tpick">
      <div className="chips" role="group">
        {chip(which === "recent", "Son 2 saat", RECENT)}
        {chip(which === "today", "Bugün", wholeDay(today))}
        {chip(which === "yday", "Dün", wholeDay(yday))}
        <label className={"chip date-chip" + (which === "other" ? " on" : "")}>
          <CalendarBlank size={16} />{which === "other" && pick.kind === "day" ? fmt.dateShort(pick.day) : "Tarih"}
          <input type="date" aria-label="Gün seç" min={isoDate(first)} max={isoDate(today)}
            value={pick.kind === "day" ? isoDate(pick.day) : ""}
            onChange={(e) => { const d = fromIsoDate(e.target.value); if (d !== null) onPick(wholeDay(Math.min(today, Math.max(first, d)))); }} />
        </label>
      </div>
      {pick.kind === "day" && (
        <>
          <div className="row">
            <button className="circ" style={{ background: "var(--card)" }} aria-label="Önceki gün" disabled={pick.day <= first}
              onClick={() => onPick({ ...pick, day: addDays(pick.day, -1) })}><CaretLeft size={18} /></button>
            <span className="sm strong grow" style={{ textAlign: "center" }}>{fmt.day(pick.day, now)}</span>
            <button className="circ" style={{ background: "var(--card)" }} aria-label="Sonraki gün" disabled={pick.day >= today}
              onClick={() => onPick({ ...pick, day: addDays(pick.day, 1) })}><CaretRight size={18} /></button>
          </div>
          <div className="row hours">
            <span className="sm">Saat</span>
            <select className="inp" aria-label="Başlangıç saati" value={pick.fromH} onChange={(e) => onPick(setHours(pick, Number(e.target.value), pick.toH))}>
              {HOURS.slice(0, 24).map((h) => <option key={h} value={h}>{hh(h)}</option>)}
            </select>
            <span className="sm">–</span>
            <select className="inp" aria-label="Bitiş saati" value={pick.toH} onChange={(e) => onPick(setHours(pick, pick.fromH, Number(e.target.value)))}>
              {HOURS.slice(1).map((h) => <option key={h} value={h} disabled={h <= pick.fromH}>{hh(h)}</option>)}
            </select>
            {(pick.fromH !== 0 || pick.toH !== 24) && <button className="chip" onClick={() => onPick(setHours(pick, 0, 24))}>Tüm gün</button>}
          </div>
        </>
      )}
    </div>
  );
}
