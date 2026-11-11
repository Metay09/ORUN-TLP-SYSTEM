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

/**
 * Hayvanın kendi alışkanlığıyla karşılaştırma (sahip 2026-11-11: hareket
 * özetini geliştir). Önceki günlerin her biri için aynı 24 saatlik pencere;
 * en az %75'i ölçülmüş gün sayılır. En az 3 böyle gün yoksa karşılaştırma yok.
 * Oran = hareketli / ölçülen süre (ölçülmeyen zaman hareketsiz sayılmaz).
 */
export interface ActivityBaseline { share: number; usual: number; days: number; change: number; }

export function activityBaseline(pts: Observation[], end: number, back = 7): ActivityBaseline | null {
  const cur = activityCoverage24h(pts, end);
  if (cur.coveredMs < 0.75 * DAY) return null;
  const shares: number[] = [];
  for (let k = 1; k <= back; k++) {
    const c = activityCoverage24h(pts, end - k * DAY);
    if (c.coveredMs >= 0.75 * DAY) shares.push(c.activeMs / c.coveredMs);
  }
  if (shares.length < 3) return null;
  const usual = shares.reduce((a, b) => a + b, 0) / shares.length;
  const share = cur.activeMs / cur.coveredMs;
  return { share, usual, days: shares.length, change: usual > 0 ? (share - usual) / usual : 0 };
}

/** Index of the hour (0..23 in hourlyActivity's window) with the most movement, or null. */
export function busiestHour(hours: (number | null)[]): number | null {
  let best: number | null = null;
  hours.forEach((v, i) => { if (v !== null && v >= 0.05 && (best === null || v > hours[best]!)) best = i; });
  return best;
}

/**
 * Pil gidişatı (sahip 2026-11-11: pili geliştir): son 24 saatin ortalaması ile
 * iki gün önceki 24 saatin ortalaması (tam gün ortalaması gece-gündüz güneş
 * salınımını yok eder) ve son 24 saatte gündüz şarjı. Az veriyle eğilim yok.
 * daysLeft yalnız belirgin düşüşte (günde %0,5'ten fazla) verilir; tahmindir.
 */
export interface BatteryOutlook {
  perDay: number | null;       // % per day; negative = draining
  daysLeft: number | null;
  charged24h: number | null;   // largest rise (% points) in the last 24 h
  change24h: number | null;    // last minus first within the last 24 h
}

export function batteryOutlook(series: { t: number; p: number }[], now: number): BatteryOutlook {
  const s = series.filter((x) => Number.isFinite(x.p) && x.t <= now).sort((a, b) => a.t - b.t);
  // Whole-day means cancel the day/night (solar) swing: today vs two days ago.
  const mean = (from: number, to: number) => {
    const w = s.filter((x) => x.t > from && x.t <= to);
    return w.length >= 4 ? w.reduce((a, x) => a + x.p, 0) / w.length : null;
  };
  const m0 = mean(now - DAY, now), m2 = mean(now - 3 * DAY, now - 2 * DAY);
  const perDay = m0 !== null && m2 !== null && s.filter((x) => x.t > now - 3 * DAY).length >= 10 ? (m0 - m2) / 2 : null;
  const lastP = s.length ? s[s.length - 1].p : null;
  const daysLeft = perDay !== null && perDay < -0.5 && lastP !== null ? Math.max(0, lastP / -perDay) : null;
  const day = s.filter((x) => x.t >= now - DAY);
  let charged: number | null = null;
  if (day.length >= 2) {
    let lo = day[0].p, best = 0;
    for (const x of day) { lo = Math.min(lo, x.p); best = Math.max(best, x.p - lo); }
    charged = best;
  }
  return {
    perDay, daysLeft, charged24h: charged,
    change24h: day.length >= 2 ? day[day.length - 1].p - day[0].p : null,
  };
}
