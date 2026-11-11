// Herd view model: located records -> one Animal per asset, with plain
// Turkish status. Pure (no DOM); tested in test/herd.test.ts.
//
// Freshness is the RECORD time (when the collar measured), never the time the
// record reached the phone (core/points.ts rules). Thresholds follow the
// collar's own report period T:
//   fresh  <= max(2T, 10 min)
//   aging  <= max(6T, 60 min)
//   silent  > max(6T, 60 min)   (stale: last position kept, shown grey)
//   lost    > max(12T, 3 h)     "kayıp olabilir": many reports missed
import { bleName } from "./deviceNames.ts";
import { fmt, tr } from "./i18n/tr.ts";
import type { Animal, AnimalStatus, AssetProfile, BatteryLevel, Freshness, Observation } from "./model.ts";
import { blankProfile } from "./stores.ts";
import { zoneAt, type Fence } from "./zones.ts";
import type { RecordId } from "../core/points.ts";

const MIN = 60_000;
const HOUR = 60 * MIN;

export const epochMs = (iso: string | null) => (iso ? Date.parse(iso) : NaN);

export function periodMs(periodS: number | null | undefined): number {
  return Math.max(30, periodS || 60) * 1000;
}
export const freshLimit = (T: number) => Math.max(2 * T, 10 * MIN);
export const agingLimit = (T: number) => Math.max(6 * T, HOUR);
export const lostLimit = (T: number) => Math.max(12 * T, 3 * HOUR);

export function freshnessOf(ms: number | null, periodS: number, now: number): Freshness {
  if (ms === null) return "none";
  const T = periodMs(periodS), age = now - ms;
  if (age <= freshLimit(T)) return "fresh";
  if (age <= agingLimit(T)) return "aging";
  return "stale";
}

/** Ring fill: 1 right after the measurement, draining to 0 at the silent limit. */
export function freshRatio(ms: number | null, periodS: number, now: number): number {
  if (ms === null) return 0;
  const lim = agingLimit(periodMs(periodS));
  return Math.max(0, Math.min(1, 1 - (now - ms) / lim));
}

// Li-ion cell: ~3.50 V empty .. ~4.15 V full under light load (bench collar).
export function batteryPercent(mv: number | null): number | null {
  if (mv === null || mv <= 0) return null;
  return Math.round(Math.min(100, Math.max(0, ((mv - 3500) / 650) * 100)));
}

export function batteryOf(p: Observation | null): { level: BatteryLevel; percent: number | null } {
  if (!p || p.battery_mv === null) return { level: "unknown", percent: null };
  const pct = batteryPercent(p.battery_mv);
  if (p.battery === "CRITICAL" || (pct !== null && pct <= 5)) return { level: "critical", percent: pct };
  if (p.battery === "LOW" || (pct !== null && pct <= 20)) return { level: "low", percent: pct };
  return { level: "good", percent: pct };
}

export function activeShare(p: Observation | null): number | null {
  if (!p || p.active_s === null || !p.covered_s) return null;
  return Math.max(0, Math.min(1, p.active_s / p.covered_s));
}

export function doingOf(share: number | null): keyof typeof tr.map.doing | null {
  if (share === null) return null;
  if (share >= 0.5) return "moving";
  if (share >= 0.1) return "little";
  return "resting";
}

export function initialsOf(name: string): string {
  const parts = name.trim().split(/\s+/).filter(Boolean);
  const s = parts.length > 1 ? parts[0][0] + parts[1][0] : name.trim().slice(0, 1);
  return s.toLocaleUpperCase("tr-TR");
}

export function shortId(id: string): string {
  return id.slice(-4).toUpperCase();
}

const ORDER: AnimalStatus[] = ["battery-critical", "lost", "silent", "aging", "battery-low", "ok", "waiting"];

export interface HerdInput {
  points: Observation[];
  profiles: Record<string, AssetProfile>;
  assetOf: (device: string, time: number) => string;
  now: number;
  fence?: Fence;        // active, valid zones (none: no zone checks)
  grazing?: boolean;    // serbest otlatma on: outside is not an alarm
  /** 2d: names kept in the account, by device id (upper-case hex). */
  names?: Record<string, string>;
  /** Records without a position: the collar was heard even when GNSS found no fix. */
  unlocated?: RecordId[];
}

export function buildHerd({ points, profiles, assetOf, now, fence, names, unlocated = [] }: HerdInput): Animal[] {
  const by = new Map<string, { device: string; list: Observation[] }>();
  for (const p of points) {
    const t = epochMs(p.time);
    const asset = assetOf(p.tracker, Number.isFinite(t) ? t : now);
    let e = by.get(asset);
    if (!e) { e = { device: p.tracker, list: [] }; by.set(asset, e); }
    e.list.push(p);
  }
  // Newest record without a position, per asset (the collar was heard).
  const heardBy = new Map<string, number>(), heardAll = new Map<string, number[]>();
  const faultBy = new Map<string, boolean>();   // newest record without a position: GNSS failed / no GNSS time
  for (const r of unlocated) {
    // A collar whose GNSS never gave time has no record time: when it reached the gateway is when it was heard.
    const own = epochMs(r.time), t = Number.isFinite(own) ? own : epochMs(r.received ?? null);
    if (!Number.isFinite(t) || t > now + 10 * MIN) continue;
    const asset = assetOf(r.tracker, t);
    if ((heardBy.get(asset) ?? -Infinity) < t) {
      heardBy.set(asset, t);
      faultBy.set(asset, !Number.isFinite(own) || (r.gnssFailures ?? 0) > 0);
    }
    if (!heardAll.has(asset)) heardAll.set(asset, []);
    heardAll.get(asset)!.push(t);
    if (!by.has(asset)) by.set(asset, { device: r.tracker, list: [] });
  }
  // Named animals without records yet still appear (waiting for a first fix).
  for (const id of Object.keys(profiles)) if (!by.has(id)) by.set(id, { device: id, list: [] });

  const herd: Animal[] = [];
  for (const [id, { list }] of by) {
    const timed = list.filter((p) => Number.isFinite(epochMs(p.time))).sort((a, b) => epochMs(a.time) - epochMs(b.time));
    const last = timed.length ? timed[timed.length - 1] : (list.length ? list[list.length - 1] : null);
    const lastTime = last && Number.isFinite(epochMs(last.time)) ? epochMs(last.time) : null;
    const periodS = last?.period_s ?? 60;
    const unloc = heardBy.get(id) ?? null;
    const lastReport = unloc !== null && (lastTime === null || unloc > lastTime) ? unloc : lastTime;
    // Heard recently, but the position is old: GNSS finds no fix (indoors, under cover).
    const posFresh = freshnessOf(lastTime, periodS, now);
    const noFix = lastReport !== null && lastReport !== lastTime &&
      (posFresh === "stale" || posFresh === "none") && freshnessOf(lastReport, periodS, now) !== "stale";
    const freshness = noFix ? freshnessOf(lastReport, periodS, now) : posFresh;
    const gpsFault = noFix && (faultBy.get(id) ?? false);
    // Solar collars recharge by day: the level is the worst of the last 24 h
    // (what the night will bring), the percentage is the newest record.
    const bat = batteryOf(last);
    if (lastTime !== null) {
      const rank = { unknown: 0, good: 1, low: 2, critical: 3 } as const;
      for (let i = timed.length - 1; i >= 0 && epochMs(timed[i].time) >= lastTime - 24 * HOUR; i--) {
        const b = batteryOf(timed[i]);
        if (rank[b.level] > rank[bat.level]) bat.level = b.level;
      }
    }
    const profile = profiles[id] ?? blankProfile(id);
    const device = last?.tracker ?? by.get(id)?.device ?? (profiles[id] ? id : null);
    // The account's name wins; then the phone's own; else the Bluetooth name.
    const given = (device && names?.[device.toUpperCase()]) || profile.name.trim();
    const named = Boolean(given);
    const name = named ? given : bleName(device ?? id);
    const heardAt = lastReport ?? lastTime;
    const lost = heardAt !== null && now - heardAt > lostLimit(periodMs(periodS));
    // Geofence alarm ownership belongs to the TRACKER, not to the app's
    // history/position projection. Keep geometric membership informational.
    const zone = fence && last ? zoneAt(fence, last)?.name ?? null : null;

    let status: AnimalStatus;
    if (!last && !noFix) status = "waiting";
    else if (bat.level === "critical") status = "battery-critical";
    else if (lost) status = "lost";
    else if (noFix) status = "aging";
    else if (freshness === "stale") status = "silent";
    else if (freshness === "aging") status = "aging";
    else if (bat.level === "low") status = "battery-low";
    else status = "ok";

    let seen: string, short: string;
    if (noFix) {
      seen = (gpsFault ? tr.status.gpsFault : tr.status.noFix)(fmt.ago(lastReport!, now), lastTime !== null ? fmt.ago(lastTime, now) : null);
      short = tr.status.short.noFix;
    }
    else if (!last) { seen = tr.status.waiting; short = tr.status.short.waiting; }
    else if (lastTime === null) { seen = tr.status.noClock; short = "?"; }
    else if (lost) { seen = tr.status.lostFor(fmt.since(heardAt!, now)); short = tr.status.short.lost; }
    else if (freshness === "fresh" || freshness === "aging") { seen = tr.status.seenAgo(fmt.ago(lastTime, now)); short = fmt.ago(lastTime, now); }
    else { seen = tr.status.silentFor(fmt.since(heardAt!, now)); short = tr.status.short.silent(fmt.duration(now - heardAt!)); }
    // The chip names the problem, not the time, when the battery is the issue.
    if (status === "battery-critical") short = tr.status.batteryCritical;
    else if (status === "battery-low") short = tr.status.batteryLow;

    herd.push({
      id, device, profile, name, named, initials: named ? initialsOf(name) : "", color: profile.color,
      points: timed, last, lastTime, lastReport, noFix, gpsFault, heardOnly: (heardAll.get(id) ?? []).sort((x, y) => x - y), periodS, freshness,
      freshRatio: freshRatio(lastTime, periodS, now), status, seen, short,
      battery: bat.level, batteryPercent: bat.percent,
      activeShare: freshness === "fresh" ? activeShare(last) : null,
      zone, outside: false, outSince: null, grazing: false,
      attention: ORDER.length - ORDER.indexOf(status),
    });
  }
  // Needs a look first, then most recently seen, then by name.
  return herd.sort((a, b) => b.attention - a.attention || (b.lastTime ?? 0) - (a.lastTime ?? 0) ||
    a.name.localeCompare(b.name, "tr"));
}

export function needsLook(a: Animal): boolean {
  return a.status !== "ok" && a.status !== "waiting";
}

/** One calm sentence for the map sheet and the herd page. */
export function herdSummary(herd: Animal[]): string {
  const located = herd.filter((a) => a.last);
  if (!herd.length) return tr.herd.summaryEmpty;
  if (!located.length) return tr.herd.summaryWaiting(herd.length);
  const fresh = herd.filter((a) => a.freshness === "fresh").length;
  const look = herd.filter(needsLook).length;
  if (look) return tr.herd.summaryLook(herd.length, look);
  if (fresh === herd.length) return tr.herd.summaryAllFresh(herd.length);
  if (fresh === 0) return tr.herd.summaryNoneFresh(herd.length);
  return tr.herd.summarySome(herd.length, fresh);
}

/** The second line under the summary: who needs a look, by name. */
export function attentionLine(herd: Animal[], now: number): string {
  return herd.filter(needsLook).slice(0, 3).map((a) => `${a.name} ${statusWord(a, now)}`).join(" · ");
}

export function statusWord(a: Animal, now: number): string {
  switch (a.status) {
    case "battery-critical": return "pil bitiyor";
    case "battery-low": return "pil azalıyor";
    case "lost": return "kayıp olabilir";
    case "silent": return a.lastTime ? `${fmt.since(a.lastTime, now)} sessiz` : "sessiz";
    case "aging": return a.lastTime ? fmt.ago(a.lastTime, now) : "";
    default: return "";
  }
}

export function matches(a: Animal, q: string): boolean {
  const s = q.trim().toLocaleLowerCase("tr-TR");
  if (!s) return true;
  const hay = [a.name, a.profile.tag, a.device ?? "", a.device ? shortId(a.device) : ""].join(" ").toLocaleLowerCase("tr-TR");
  return hay.includes(s);
}
