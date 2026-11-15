// Which stretch of time the selected animal's trail shows (owner decision
// 2026-11-16): "Son 2 saat" for a quick look, otherwise one calendar day
// with an optional hour range, like Tractive / Tracker history pickers.
export type DayPick = { kind: "day"; day: number; fromH: number; toH: number };   // day = local midnight (ms)
export type TrailPick = { kind: "recent" } | DayPick;

export const RECENT_MS = 2 * 3600_000;

export function startOfDay(ms: number): number {
  const d = new Date(ms);
  d.setHours(0, 0, 0, 0);
  return d.getTime();
}

/** Local midnight `n` days after `day` (calendar days, not 24 h steps). */
export function addDays(day: number, n: number): number {
  const d = new Date(day);
  d.setDate(d.getDate() + n);
  return startOfDay(d.getTime());
}

const at = (day: number, h: number): number => { const d = new Date(day); d.setHours(h, 0, 0, 0); return d.getTime(); };

export function wholeDay(day: number): DayPick {
  return { kind: "day", day: startOfDay(day), fromH: 0, toH: 24 };
}

/** Time window of a pick. A day never reaches past now. */
export function pickWindow(p: TrailPick, now: number): { from: number; to: number } {
  if (p.kind === "recent") return { from: now - RECENT_MS, to: now };
  const from = at(p.day, p.fromH);
  return { from, to: Math.max(from, Math.min(now, at(p.day, p.toH) - 1)) };
}

/** A window asked for by another screen (e.g. one day of the animal). */
export function pickFromWindow(w: { from: number; to: number }): DayPick {
  const day = startOfDay(w.from);
  const fromH = new Date(w.from).getHours();
  const end = w.to + 1;
  const toH = startOfDay(end) > day ? 24 : Math.max(fromH + 1, Math.ceil((end - day) / 3600_000));
  return { kind: "day", day, fromH, toH: Math.min(24, toH) };
}

/** Hours a day pick may still change to: from < to, both within 0..24. */
export function setHours(p: DayPick, fromH: number, toH: number): DayPick {
  const f = Math.max(0, Math.min(23, fromH));
  const t = Math.max(f + 1, Math.min(24, toH));
  return { ...p, fromH: f, toH: t };
}

/** `YYYY-MM-DD` for `<input type="date">`, in local time. */
export function isoDate(ms: number): string {
  const d = new Date(ms);
  return `${d.getFullYear()}-${String(d.getMonth() + 1).padStart(2, "0")}-${String(d.getDate()).padStart(2, "0")}`;
}

export function fromIsoDate(s: string): number | null {
  const m = /^(\d{4})-(\d{2})-(\d{2})$/.exec(s);
  if (!m) return null;
  const d = new Date(Number(m[1]), Number(m[2]) - 1, Number(m[3]));
  return Number.isFinite(d.getTime()) ? d.getTime() : null;
}
