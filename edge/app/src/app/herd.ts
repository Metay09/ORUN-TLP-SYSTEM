// Herd view model: located records -> one Animal per asset, with plain
// Turkish status. Pure (no DOM); tested in test/herd.test.ts.
//
// Freshness is the RECORD time (when the collar measured), never the time the
// record reached the phone (core/points.ts rules). Thresholds follow the
// collar's own report period T:
//   fresh  <= T + min(T, 2 min)  (owner 2026-11-14: the next report plus the
//                               time to find the fix and reach the phone;
//                               1 min -> 2, 5 min -> 7, 60 min -> 62)
//   aging  <= max(3T, 30 min), capped at 24 h
//   silent  > agingLimit(T)    (stale: last position kept, shown grey)
//   lost    > 2 * agingLimit(T), unless outside its allowed zones
import { bleName } from "./deviceNames.ts";
import { fmt, tr } from "./i18n/tr.ts";
import type { Animal, AnimalStatus, AssetProfile, BatteryLevel, Freshness, Observation } from "./model.ts";
import { blankProfile } from "./stores.ts";
import { outsideEpisodes, zoneAt, type Fence } from "./zones.ts";
import type { RecordId } from "../core/points.ts";

const MIN = 60_000;
const HOUR = 60 * MIN;

export const epochMs = (iso: string | null) => (iso ? Date.parse(iso) : NaN);

export function periodMs(periodS: number | null | undefined): number {
  return Math.max(30, periodS || 60) * 1000;
}
// Fresh until the next report is due plus the time it takes to arrive: the
// GNSS fix and the trip to the phone. Exactly T made a 5-min collar turn
// yellow ("bakın") for a minute every cycle (owner's phone, 2026-11-14:
// 07:16 -> 07:22). The slack is one more report for short intervals and
// never more than 2 minutes. A visual status, not a notification gate.
export const freshLimit = (T: number) => T + Math.min(T, 2 * MIN);
// Owner decision 2026-11-13: "no news" is counted in reports, because the
// report interval changes (2 min .. 14 h): three reports in a row missing,
// but never before 30 minutes and never later than 24 hours. "May be lost"
// is twice that.
export const agingLimit = (T: number) => Math.min(Math.max(3 * T, HOUR / 2), 24 * HOUR);
export const lostLimit = (T: number) => 2 * agingLimit(T);

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

/**
 * The same table as the firmware (battery_policy.cpp batteryPercentEstimate):
 * Meshtastic's LiPo rest-voltage table (4190 = 100 %, 4050 = 90 %, nRF52
 * variant with 3.40 V empty). A FULL battery reads 100 % from the collar's
 * power state (readingPercent), not from this curve (owner 2026-11-12). With
 * 4.05 V stretched to 100 % a battery on its own read "100 %" for its first
 * ~10 % of use (bench 2026-11-12, Hayvan 2).
 */
const CURVE: [number, number][] = [[3400, 0], [3440, 10], [3480, 20], [3530, 30], [3630, 40],
  [3720, 50], [3800, 60], [3890, 70], [3990, 80], [4050, 90], [4190, 100]];
/** The same curve without rounding (battery-life fit: whole percents are too coarse over a few hours). */
export function batteryPercentExact(mv: number): number {
  if (mv <= CURVE[0][0]) return 0;
  for (let i = 1; i < CURVE.length; i++) {
    const [hm, hp] = CURVE[i], [lm, lp] = CURVE[i - 1];
    if (mv <= hm) return lp + ((mv - lm) * (hp - lp)) / (hm - lm);
  }
  return 100;
}

export function batteryPercent(mv: number | null): number | null {
  if (mv === null || mv <= 0) return null;
  if (mv <= CURVE[0][0]) return 0;
  for (let i = 1; i < CURVE.length; i++) {
    const [hm, hp] = CURVE[i], [lm, lp] = CURVE[i - 1];
    if (mv <= hm) return lp + Math.floor(((mv - lm) * (hp - lp) + (hm - lm) / 2) / (hm - lm));
  }
  return 100;
}

/** A reading's percent with the collar's own word: FULL is 100 %, no battery has none. */
export function readingPercent(b: { mv: number | null; power?: string | null }): number | null {
  if (b.power === "FULL") return 100;
  if (b.power === "NO_BATTERY") return null;
  return batteryPercent(b.mv);
}

/** On an outside supply (USB) or without a cell: says nothing about the battery's own drain. */
export function externallyPowered(b: { power?: string | null; usb?: boolean | null }): boolean {
  return b.usb === true || b.power === "NO_BATTERY";
}

export function batteryOf(p: Observation | null): { level: BatteryLevel; percent: number | null } {
  if (!p || p.battery_mv === null) return { level: "unknown", percent: null };
  if (p.power === "NO_BATTERY") return { level: "unknown", percent: null };
  const pct = readingPercent({ mv: p.battery_mv, power: p.power });
  if (p.power === "FULL" || p.power === "CHARGING") return { level: "good", percent: pct };
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

const ORDER: AnimalStatus[] = ["battery-critical", "lost", "out", "silent", "aging", "battery-low", "ok", "waiting"];

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
  /** The report interval each collar confirmed and when (by upper-case device id). */
  intervals?: Record<string, { s: number; at: number }>;
}

export function buildHerd({ points, profiles, assetOf, now, fence, grazing = false, names, unlocated = [], intervals }: HerdInput): Animal[] {
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
  const extraBy = new Map<string, { o: Observation; t: number }[]>();   // records without a position, as reports
  for (const r of unlocated) {
    // A collar whose GNSS never gave time has no record time: when it reached the gateway is when it was heard.
    const own = epochMs(r.time), t = Number.isFinite(own) ? own : epochMs(r.received ?? null);
    if (!Number.isFinite(t) || t > now + 10 * MIN) continue;
    const asset = assetOf(r.tracker, t);
    const o: Observation = {
      tracker: r.tracker, incarnation: r.incarnation, seq: r.seq, time: r.time, lat: NaN, lon: NaN, alt_m: null,
      sats: null, hdop: null, fix_age_s: null, battery_mv: r.battery_mv ?? null, battery: r.battery ?? null,
      power: r.power ?? null, usb: r.usb ?? null,
      active_s: r.active_s ?? null, covered_s: r.covered_s ?? null, source: "NONE", period_s: r.period_s ?? 0,
      rssi: null, snr: null, received: r.received ?? null, delay_s: null, link: null,
    };
    if (!extraBy.has(asset)) extraBy.set(asset, []);
    extraBy.get(asset)!.push({ o, t });
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
    // The longer of the newest record's interval and the one the collar has
    // confirmed since: after "2 dk -> 60 dk" the collar is not "silent" for
    // the hour until its first hourly report (owner's phone, 2026-11-13).
    const tracker = (last?.tracker ?? by.get(id)?.device ?? "").toUpperCase();
    // Only a confirmation newer than the newest record counts: an old one
    // must not slow the silence check of a collar that reports faster now.
    // Outside its own zones the collar reports three times as often
    // (firmware geofence_runtime_policy: base / 3) and falls back to the base
    // interval when it is inside again or the zone is removed. A record that
    // says "20 dk" may therefore be followed by one a full base interval
    // later: allow 1.25 x base before calling it silence (3 x 0.42 = 1.26).
    // Seen on the owner's phone 2026-11-14: "Hayvan 2 sessiz · 3 rapor kaçtı".
    const set = intervals?.[tracker];
    const periodS = Math.max(last?.period_s ?? 60,
      set ? (set.at > (lastTime ?? 0) ? set.s : Math.ceil(set.s * 0.42)) : 0);
    const unloc = heardBy.get(id) ?? null;
    const lastReport = unloc !== null && (lastTime === null || unloc > lastTime) ? unloc : lastTime;
    // Heard recently, but the position is old: GNSS finds no fix (indoors, under cover).
    const posFresh = freshnessOf(lastTime, periodS, now);
    const noFix = lastReport !== null && lastReport !== lastTime &&
      (posFresh === "stale" || posFresh === "none") && freshnessOf(lastReport, periodS, now) !== "stale";
    const freshness = noFix ? freshnessOf(lastReport, periodS, now) : posFresh;
    const gpsFault = noFix && (faultBy.get(id) ?? false);
    // Every record is a report, with or without a fix: activity from those
    // with a clock, battery from all (no clock: when it reached the gateway).
    const extra = extraBy.get(id) ?? [];
    const reports = [...timed, ...extra.filter((x) => Number.isFinite(epochMs(x.o.time))).map((x) => x.o)]
      .sort((x, y) => epochMs(x.time) - epochMs(y.time));
    const withBattery = [...timed.map((o) => ({ o, t: epochMs(o.time) })), ...extra]
      .filter((x) => x.o.battery_mv !== null && Number.isFinite(x.t)).sort((x, y) => x.t - y.t);
    const newestBat = withBattery.length ? withBattery[withBattery.length - 1] : null;
    // Solar collars recharge by day: the level is the worst of the last 24 h
    // (what the night will bring), the percentage is the newest record.
    const bat = batteryOf(newestBat?.o ?? last);
    if (newestBat) {
      const rank = { unknown: 0, good: 1, low: 2, critical: 3 } as const;
      for (let i = withBattery.length - 1; i >= 0 && withBattery[i].t >= newestBat.t - 24 * HOUR; i--) {
        const b = batteryOf(withBattery[i].o);
        if (rank[b.level] > rank[bat.level]) bat.level = b.level;
      }
    }
    const batteryTrail = withBattery.filter((x) => x.t >= now - 7 * 24 * HOUR)
      .map((x) => ({ t: x.t, mv: x.o.battery_mv!, power: x.o.power ?? null, usb: x.o.usb ?? null, periodS: x.o.period_s || null }));
    const profile = profiles[id] ?? blankProfile(id);
    const device = last?.tracker ?? by.get(id)?.device ?? (profiles[id] ? id : null);
    // The account's name wins; then the phone's own; else the Bluetooth name.
    const given = (device && names?.[device.toUpperCase()]) || profile.name.trim();
    const named = Boolean(given);
    const name = named ? given : bleName(device ?? id);
    const heardAt = lastReport ?? lastTime;
    let lost = heardAt !== null && now - heardAt > lostLimit(periodMs(periodS));
    // Owner decision 2026-11-13 (v1): the app decides "outside" from the
    // positions it holds and the zones of the account, with the collar's own
    // rule (the newest record outside and 2 of the last 3). No extra packet
    // on the air. The collar's signed EVENT replaces this source later.
    const zone = fence && last ? zoneAt(fence, last)?.name ?? null : null;
    const openOut = fence
      ? outsideEpisodes(timed, fence, (p) => epochMs(p.time)).find((e) => e.to === null) ?? null
      : null;
    const outside = openOut !== null;
    // Outside the zones and no news for three reports: may be lost already.
    if (outside && !grazing && heardAt !== null && now - heardAt > agingLimit(periodMs(periodS))) lost = true;

    let status: AnimalStatus;
    if (!last && !noFix) status = "waiting";
    else if (bat.level === "critical") status = "battery-critical";
    else if (lost) status = "lost";
    else if (outside && !grazing) status = "out";
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
    if (status === "out") short = tr.status.short.out;
    if (status === "battery-critical") short = tr.status.batteryCritical;
    else if (status === "battery-low") short = tr.status.batteryLow;

    herd.push({
      id, device, profile, name, named, initials: named ? initialsOf(name) : "", color: profile.color,
      points: timed, reports, batteryTrail, batteryAt: newestBat?.t ?? null, batteryMv: newestBat?.o.battery_mv ?? null,
      batteryPower: newestBat?.o.power ?? null, batteryUsb: newestBat?.o.usb ?? null,
      last, lastTime, lastReport, noFix, gpsFault, heardOnly: (heardAll.get(id) ?? []).sort((x, y) => x - y), periodS, freshness,
      freshRatio: freshRatio(lastTime, periodS, now), status, seen, short,
      battery: bat.level, batteryPercent: bat.percent,
      activeShare: freshness === "fresh" ? activeShare(last) : null,
      zone, outside, outSince: openOut?.from ?? null, grazing: outside && grazing,
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
