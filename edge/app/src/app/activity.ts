// Per-animal summaries from the records themselves (scope §5): movement time
// (the collar's per-period active seconds), straight-line distance between
// records, per-day history and the battery curve. Pure; no invented classes —
// grazing/resting/walking appears only once collars report it.
import { pathLengthM } from "./geo.ts";
import { epochMs } from "./herd.ts";
import type { Observation } from "./model.ts";

const HOUR = 3600_000;
const DAY = 24 * HOUR;

export function dayStart(ms: number): number {
  const d = new Date(ms);
  d.setHours(0, 0, 0, 0);
  return d.getTime();
}

const timed = (pts: Observation[]) =>
  pts.map((p) => ({ p, t: epochMs(p.time) })).filter((x) => Number.isFinite(x.t));

/** 24 hourly bins ending at `end`: share of each hour in motion (null = no data). */
export function hourlyActivity(pts: Observation[], end: number): (number | null)[] {
  const start = end - DAY;
  const act = new Array<number>(24).fill(0), cov = new Array<number>(24).fill(0);
  for (const { p, t } of timed(pts)) {
    if (t <= start || t > end || p.active_s === null || !p.covered_s) continue;
    const i = Math.min(23, Math.floor((t - start) / HOUR));
    act[i] += p.active_s;
    cov[i] += p.covered_s;
  }
  return act.map((a, i) => (cov[i] ? Math.min(1, a / cov[i]) : null));
}


/**
 * Confirmed activity coverage over a rolling 24 hours.
 *
 * The collar reports active seconds and measured sensor coverage per PERIODIC
 * record.  GPS distance is deliberately not used to infer activity.
 * Reject malformed, duplicate and overlapping report periods. A report
 * straddling the 24-hour boundary is excluded, not proportionally invented.
 * Unknown time therefore includes missing/late reports and excluded fragments.
 */
export interface ActivityCoverage24h {
  activeMs: number;
  inactiveMs: number;
  unknownMs: number;
  coveredMs: number;
  reports: number;
}

export function activityCoverage24h(pts: Observation[], end: number): ActivityCoverage24h {
  const from = end - DAY;
  const rows = timed(pts).filter(({ t }) => Number.isFinite(t) && t > from && t <= end)
    .sort((a, b) => a.t - b.t);
  const seen = new Set<string>();
  let activeMs = 0, coveredMs = 0, reports = 0, priorEnd = from;
  for (const { p, t } of rows) {
    if (p.active_s === null || p.covered_s === null ||
        !Number.isSafeInteger(p.active_s) || !Number.isSafeInteger(p.covered_s) ||
        !Number.isSafeInteger(p.period_s) || p.period_s <= 0 ||
        p.active_s < 0 || p.active_s > p.covered_s ||
        p.covered_s <= 0 || p.covered_s > p.period_s) continue;
    const key = `${p.tracker}:${p.incarnation ?? ""}:${p.seq}:${t}`;
    if (seen.has(key)) continue;
    seen.add(key);
    const periodStart = t - p.period_s * 1000;
    if (periodStart < from || periodStart < priorEnd) continue;
    activeMs += p.active_s * 1000;
    coveredMs += p.covered_s * 1000;
    priorEnd = t;
    reports++;
  }
  return {
    activeMs,
    inactiveMs: coveredMs - activeMs,
    unknownMs: Math.max(0, DAY - coveredMs),
    coveredMs,
    reports,
  };
}

export interface DaySummary {
  day: number;              // local midnight, ms
  records: number;
  first: number;
  last: number;
  walkedM: number;
  activeMs: number | null;  // null: collar sent no movement summary
  points: Observation[];
}

export function daySummaries(pts: Observation[]): DaySummary[] {
  const by = new Map<number, { p: Observation; t: number }[]>();
  for (const x of timed(pts)) {
    const d = dayStart(x.t);
    if (!by.has(d)) by.set(d, []);
    by.get(d)!.push(x);
  }
  const out: DaySummary[] = [];
  for (const [day, list] of by) {
    list.sort((a, b) => a.t - b.t);
    let active = 0, any = false;
    for (const { p } of list) if (p.active_s !== null && p.covered_s) { active += p.active_s; any = true; }
    out.push({
      day, records: list.length, first: list[0].t, last: list[list.length - 1].t,
      walkedM: pathLengthM(list.map(({ p, t }) => ({ lat: p.lat, lon: p.lon, t }))),
      activeMs: any ? active * 1000 : null,
      points: list.map((x) => x.p),
    });
  }
  return out.sort((a, b) => b.day - a.day);
}

/** Battery voltage over the last `days` days, one value per record. */
export function batterySeries(pts: Observation[], now: number, days = 7): { t: number; mv: number }[] {
  const from = now - days * DAY;
  return timed(pts).filter(({ p, t }) => t >= from && p.battery_mv !== null)
    .map(({ p, t }) => ({ t, mv: p.battery_mv! })).sort((a, b) => a.t - b.t);
}

export function medianDelayS(pts: Observation[]): number | null {
  const d = pts.map((p) => p.delay_s).filter((x): x is number => x !== null).sort((a, b) => a - b);
  return d.length ? d[Math.floor(d.length / 2)] : null;
}

/** Records within [from, to], oldest first. */
export function inRange(pts: Observation[], from: number, to = Infinity): Observation[] {
  return pts.filter((p) => { const t = epochMs(p.time); return t >= from && t <= to; });
}
