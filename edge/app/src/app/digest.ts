// Olaylar özeti: ham olay listesinden işlenmiş üç bölüm.
//  - "Şimdi": hayvan başına TEK kart; süren sorunları birleştirir, tek öneri verir.
//  - "Gün gün": kapanmış olaylar hayvan ve tür bazında birleştirilir ("3 kez
//    haber kesildi, toplam 4 sa 10 dk"), günün başlığı kaç hayvanın sorunsuz
//    olduğunu söyler.
//  - "Sistem": bağlantı / aktarım satırları tür başına tek satıra iner.
// Yeni olay ÜRETMEZ: yalnız deriveEvents'in verdiğini özetler.
import { isActive } from "./events.ts";
import { fmt, tr } from "./i18n/tr.ts";
import type { Animal, AppEvent, EventKind, Severity } from "./model.ts";

const T = tr.events.digest;
const RANK: Record<Severity, number> = { danger: 0, warn: 1, info: 2, ok: 3 };
const SYSTEM = new Set<EventKind>(["link", "records", "handoff", "upload", "keys"]);
// What to mention (and advise on) first when one animal has several problems.
const ORDER: EventKind[] = ["lost", "out", "battery-critical", "still", "silent", "battery-low", "station-full", "server-down"];
const DAY = 86_400_000;

export interface NowItem {
  key: string; asset: string | null; name: string; severity: Severity;
  reasons: string[]; advice: string; since: number; ids: string[];
  kind: EventKind;   // the heaviest reason; decides the order and the advice
}
export interface DayLine { asset: string | null; name: string; severity: Severity; text: string; at: number; }
export interface SystemLine { text: string; count: number; last: number; }
export interface DayDigest { start: number; label: string; headline: string; lines: DayLine[]; system: SystemLine[]; }
export interface Digest { now: NowItem[]; days: DayDigest[]; }

const worst = (a: Severity, b: Severity): Severity => (RANK[a] <= RANK[b] ? a : b);
const dayStart = (ms: number): number => { const d = new Date(ms); d.setHours(0, 0, 0, 0); return d.getTime(); };

function reason(e: AppEvent, a: Animal | undefined, now: number): string {
  const since = fmt.since(e.at, now), p = a?.batteryPercent ?? null;
  switch (e.kind) {
    case "lost": return T.reason.lost(since);
    case "silent": return T.reason.silent(since);
    case "out": return T.reason.out(since);
    case "still": return T.reason.still(since);
    case "battery-critical": return T.reason.batteryCritical(p);
    case "battery-low": return T.reason.batteryLow(p);
    default: return e.detail || e.title;
  }
}

function advice(kind: EventKind): string {
  switch (kind) {
    case "lost": return T.advice.lost;
    case "out": return T.advice.out;
    case "battery-critical": return T.advice.batteryCritical;
    case "still": return T.advice.still;
    case "silent": return T.advice.silent;
    case "battery-low": return T.advice.batteryLow;
    case "station-full": return T.advice.stationFull;
    case "server-down": return T.advice.serverDown;
    default: return "";
  }
}

function nowItems(events: AppEvent[], herd: Map<string, Animal>, now: number): NowItem[] {
  const groups = new Map<string, AppEvent[]>();
  for (const e of events) {
    if (!isActive(e)) continue;
    const key = e.asset ?? `sys:${e.kind}`;
    groups.set(key, [...(groups.get(key) ?? []), e]);
  }
  const out: NowItem[] = [];
  for (const [key, list] of groups) {
    const rank = (e: AppEvent) => { const i = ORDER.indexOf(e.kind); return i < 0 ? ORDER.length : i; };
    list.sort((x, y) => rank(x) - rank(y));
    const a = list[0].asset ? herd.get(list[0].asset) : undefined;
    // One reason per kind (silent and lost never both; keep the first of each).
    const seenKind = new Set<EventKind>(), reasons: string[] = [];
    for (const e of list) {
      if (seenKind.has(e.kind)) continue;
      seenKind.add(e.kind);
      reasons.push(reason(e, a, now));
    }
    out.push({
      key, asset: list[0].asset, name: a?.name ?? list[0].title,
      severity: list.reduce<Severity>((s, e) => worst(s, e.severity), "ok"),
      reasons: a ? reasons : reasons.slice(0, 1), advice: advice(list[0].kind), kind: list[0].kind,
      since: Math.min(...list.map((e) => e.at)), ids: list.map((e) => e.id),
    });
  }
  return out.sort((x, y) => RANK[x.severity] - RANK[y.severity] || ORDER.indexOf(x.kind) - ORDER.indexOf(y.kind) || x.since - y.since);
}

type Family = "gap" | "still" | "out" | "battery" | "note";
const familyOf = (k: EventKind): Family | null =>
  k === "silent" || k === "lost" ? "gap" : k === "still" ? "still" : k === "out" ? "out"
    : k === "battery-low" || k === "battery-critical" ? "battery" : k === "note" ? "note" : null;

function mergeLine(family: Family, list: AppEvent[]): string {
  const total = list.reduce((s, e) => s + ((e.end ?? e.at) - e.at), 0);
  const longest = list.reduce((x, e) => ((e.end ?? e.at) - e.at > (x.end ?? x.at) - x.at ? e : x), list[0]);
  const a = fmt.clock(longest.at), b = fmt.clock(longest.end ?? longest.at);
  switch (family) {
    case "gap": return list.length === 1 ? T.gapOne(a, b, fmt.duration(total)) : T.gapMany(list.length, fmt.duration(total), a, b);
    case "still": return list.length === 1 ? T.stillOne(fmt.duration(total), a, b) : T.stillMany(list.length, fmt.duration(total));
    case "out": return list.length === 1 ? T.outOne(fmt.duration(total), a, b) : T.outMany(list.length, fmt.duration(total));
    case "battery": return list.some((e) => e.kind === "battery-critical") ? T.batteryCritical : T.batteryLow;
    case "note": return list.map((e) => T.note(e.detail)).join(" · ");
  }
}

export function digest(events: AppEvent[], herdList: Animal[], now: number, days = 7): Digest {
  const herd = new Map(herdList.map((a) => [a.id, a]));
  const today = dayStart(now);
  const out: DayDigest[] = [];
  for (let i = 0; i < days; i++) {
    const start = dayStart(today - i * DAY + DAY / 2), end = dayStart(start + DAY + DAY / 2);
    const inDay = events.filter((e) => e.at >= start && e.at < end);
    // Closed animal episodes, merged per animal and family.
    const groups = new Map<string, { family: Family; list: AppEvent[] }>();
    for (const e of inDay) {
      const family = familyOf(e.kind);
      if (!family || !e.asset || (e.alert && e.end === null)) continue;
      const key = `${e.asset}:${family}`;
      const g = groups.get(key) ?? { family, list: [] };
      g.list.push(e);
      groups.set(key, g);
    }
    const lines: DayLine[] = [...groups.values()].map(({ family, list }) => ({
      asset: list[0].asset, name: herd.get(list[0].asset!)?.name ?? list[0].title,
      severity: family === "note" ? "info" : list.reduce<Severity>((s, e) => worst(s, e.severity), "ok"),
      text: mergeLine(family, list), at: Math.min(...list.map((e) => e.at)),
    })).sort((x, y) => RANK[x.severity] - RANK[y.severity] || x.at - y.at);
    // System book lines: one per kind, with the newest wording.
    const byKind = new Map<EventKind, SystemLine>();
    for (const e of inDay) {
      if (e.alert || !SYSTEM.has(e.kind)) continue;
      const s = byKind.get(e.kind);
      if (!s) byKind.set(e.kind, { text: e.title, count: 1, last: e.at });
      else { s.count++; if (e.at > s.last) { s.last = e.at; s.text = e.title; } }
    }
    const system = [...byKind.values()].sort((x, y) => y.last - x.last)
      .map((s) => (s.count > 1 ? { ...s, text: T.times(s.text, s.count) } : s));
    if (!lines.length && !system.length && i > 0) continue;
    const hit = new Set(lines.filter((l) => l.severity !== "info").map((l) => l.asset)).size;
    out.push({
      start, label: fmt.day(start + DAY / 2, now), lines, system,
      headline: hit ? T.headline(hit, Math.max(0, herdList.length - hit)) : T.quiet,
    });
  }
  return { now: nowItems(events, herd, now), days: out };
}
