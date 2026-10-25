// Herd view model: one entry per collar from the phone ledger's points, with
// plain Turkish status sentences. Pure functions (no DOM), tested in
// test/herd.test.ts.
//
// Freshness is the RECORD time (when the collar measured), never the time the
// record reached the phone (points.ts rules). Thresholds follow the collar's
// own reporting period T: fresh <= max(2T, 10 min), aging <= max(6T, 60 min),
// then stale. A stale animal keeps its last known position, shown greyed.
import type { Point } from "../core/points.ts";

export type Freshness = "fresh" | "aging" | "stale" | "none";
export type BatteryLevel = "good" | "low" | "critical" | "unknown";

export interface Profile { name?: string; color?: string; note?: string; }
export type Profiles = Record<string, Profile>;

export interface Animal {
  id: string;               // collar (tracker) id, 16 hex
  name: string;
  named: boolean;           // the owner gave it a name (else "Tasma 1AA3")
  color: string;
  last: Point | null;       // newest located record
  lastTime: number | null;  // epoch ms of that record
  count: number;            // located records on the phone
  freshness: Freshness;
  seen: string;             // "8 dk önce görüldü" / "2 saattir haber yok"
  battery: BatteryLevel;
  batteryText: string | null;
  batteryPercent: number | null;
  movement: string | null;  // "hareketli" / "az hareketli" / "dinleniyor"
  attention: number;        // sort key: higher = needs a look first
  trail: Point[];           // last 24 h, oldest first
}

const MIN = 60_000;
const HOUR = 60 * MIN;
const DAY = 24 * HOUR;

// Warm, distinguishable marker colours (no red: red means alarm).
const COLORS = ["#3E6B3A", "#A8552E", "#2F6F8F", "#8A6D1F", "#6B4E8A", "#2E7D6B", "#9A4F6A", "#5B6B2E"];

export function shortId(id: string): string {
  return id.slice(-4).toUpperCase();
}

export function defaultName(id: string): string {
  return `Tasma ${shortId(id)}`;
}

export function colorFor(id: string): string {
  let h = 0;
  for (const c of id) h = (h * 31 + c.charCodeAt(0)) >>> 0;
  return COLORS[h % COLORS.length];
}

const MONTHS = ["Oca", "Şub", "Mar", "Nis", "May", "Haz", "Tem", "Ağu", "Eyl", "Eki", "Kas", "Ara"];

export function clock(ms: number): string {
  const d = new Date(ms);
  return `${String(d.getHours()).padStart(2, "0")}:${String(d.getMinutes()).padStart(2, "0")}`;
}

function sameDay(a: Date, b: Date): boolean {
  return a.getFullYear() === b.getFullYear() && a.getMonth() === b.getMonth() && a.getDate() === b.getDate();
}

/** "az önce", "8 dk önce", "3 sa önce", "dün 14:20", "12 Eki 09:05". */
export function ago(ms: number, now: number): string {
  const d = Math.max(0, now - ms);
  if (d < MIN) return "az önce";
  if (d < HOUR) return `${Math.floor(d / MIN)} dk önce`;
  const then = new Date(ms), today = new Date(now);
  if (d < 12 * HOUR || sameDay(then, today)) return `${Math.floor(d / HOUR)} sa önce`;
  const yesterday = new Date(now - DAY);
  if (sameDay(then, yesterday)) return `dün ${clock(ms)}`;
  return `${then.getDate()} ${MONTHS[then.getMonth()]} ${clock(ms)}`;
}

/** "40 dakikadır", "2 saattir", "3 gündür" (suffixes fixed per unit). */
export function since(ms: number, now: number): string {
  const d = Math.max(0, now - ms);
  if (d < HOUR) return `${Math.max(1, Math.floor(d / MIN))} dakikadır`;
  if (d < 2 * DAY) return `${Math.floor(d / HOUR)} saattir`;
  return `${Math.floor(d / DAY)} gündür`;
}

export function freshnessOf(ms: number | null, periodS: number, now: number): Freshness {
  if (ms === null) return "none";
  const t = Math.max(30, periodS || 60) * 1000;
  const age = now - ms;
  if (age <= Math.max(2 * t, 10 * MIN)) return "fresh";
  if (age <= Math.max(6 * t, HOUR)) return "aging";
  return "stale";
}

// Li-ion cell: ~3.50 V empty .. ~4.15 V full under light load (bench collar).
export function batteryPercent(mv: number | null): number | null {
  if (mv === null || mv <= 0) return null;
  return Math.round(Math.min(100, Math.max(0, ((mv - 3500) / 650) * 100)));
}

export function batteryOf(p: Point | null): { level: BatteryLevel; text: string | null; percent: number | null } {
  if (!p || p.battery_mv === null) return { level: "unknown", text: null, percent: null };
  const pct = batteryPercent(p.battery_mv);
  if (p.battery === "CRITICAL" || (pct !== null && pct <= 5))
    return { level: "critical", text: "Pil bitmek üzere · bugün şarj edin", percent: pct };
  if (p.battery === "LOW" || (pct !== null && pct <= 20))
    return { level: "low", text: `Pil azalıyor · %${pct ?? "?"}`, percent: pct };
  return { level: "good", text: `Pil iyi · %${pct ?? "?"}`, percent: pct };
}

export function movementOf(p: Point | null): string | null {
  if (!p || p.active_s === null || !p.covered_s) return null;
  const r = p.active_s / p.covered_s;
  if (r >= 0.5) return "hareketli";
  if (r >= 0.1) return "az hareketli";
  return "dinleniyor";
}

const epochMs = (iso: string | null) => (iso ? Date.parse(iso) : NaN);

export function buildHerd(points: Point[], profiles: Profiles, now: number): Animal[] {
  const by = new Map<string, Point[]>();
  for (const p of points) {
    if (!by.has(p.tracker)) by.set(p.tracker, []);
    by.get(p.tracker)!.push(p);
  }
  for (const id of Object.keys(profiles)) if (!by.has(id)) by.set(id, []);
  const herd: Animal[] = [];
  for (const [id, list] of by) {
    const timed = list.filter((p) => Number.isFinite(epochMs(p.time)))
      .sort((a, b) => epochMs(a.time) - epochMs(b.time));
    const last = timed.length ? timed[timed.length - 1] : (list.length ? list[list.length - 1] : null);
    const lastTime = last && Number.isFinite(epochMs(last.time)) ? epochMs(last.time) : null;
    const freshness = freshnessOf(lastTime, last?.period_s ?? 60, now);
    const bat = batteryOf(last);
    const prof = profiles[id] ?? {};
    const seen = lastTime === null
      ? (last ? "Konum var · saat bilgisi yok" : "İlk konum bekleniyor")
      : freshness === "fresh" ? `${capital(ago(lastTime, now))} görüldü`
      : `${capital(since(lastTime, now))} haber yok`;
    const attention =
      (bat.level === "critical" ? 50 : 0) + (freshness === "stale" ? 30 : freshness === "aging" ? 20 : 0) +
      (bat.level === "low" ? 10 : 0) + (freshness === "none" ? 5 : 0);
    herd.push({
      id, name: prof.name?.trim() || defaultName(id), named: Boolean(prof.name?.trim()), color: prof.color || colorFor(id),
      last, lastTime, count: list.length, freshness, seen,
      battery: bat.level, batteryText: bat.text, batteryPercent: bat.percent,
      movement: freshness === "fresh" ? movementOf(last) : null,
      attention,
      trail: timed.filter((p) => lastTime !== null && epochMs(p.time) >= lastTime - DAY),
    });
  }
  // Needs a look first, then most recently seen, then by name.
  return herd.sort((a, b) => b.attention - a.attention || (b.lastTime ?? 0) - (a.lastTime ?? 0) ||
    a.name.localeCompare(b.name, "tr"));
}

function capital(s: string): string {
  return s ? s.charAt(0).toLocaleUpperCase("tr-TR") + s.slice(1) : s;
}

/** One calm sentence for the collapsed sheet. */
export function herdSummary(herd: Animal[]): string {
  const located = herd.filter((a) => a.last);
  if (!herd.length) return "Henüz hayvan yok";
  if (!located.length) return `${herd.length} tasma · ilk konum bekleniyor`;
  const fresh = herd.filter((a) => a.freshness === "fresh").length;
  const look = herd.filter((a) => a.attention >= 20 || a.battery !== "good" && a.battery !== "unknown").length;
  if (look) return `${herd.length} hayvan · ${look} tanesine bakın`;
  if (fresh === herd.length) return `${herd.length} hayvan · hepsi yakın zamanda görüldü`;
  if (fresh === 0) return `${herd.length} hayvan · yeni konum gelmedi`;
  return `${herd.length} hayvan · ${fresh} tanesi yakın zamanda görüldü`;
}

// ---- distance / direction from the phone ----------------------------------
const R = 6371000;
const rad = (d: number) => (d * Math.PI) / 180;

export function distanceM(lat1: number, lon1: number, lat2: number, lon2: number): number {
  const dLat = rad(lat2 - lat1), dLon = rad(lon2 - lon1);
  const a = Math.sin(dLat / 2) ** 2 + Math.cos(rad(lat1)) * Math.cos(rad(lat2)) * Math.sin(dLon / 2) ** 2;
  return 2 * R * Math.asin(Math.min(1, Math.sqrt(a)));
}

export function bearingDeg(lat1: number, lon1: number, lat2: number, lon2: number): number {
  const y = Math.sin(rad(lon2 - lon1)) * Math.cos(rad(lat2));
  const x = Math.cos(rad(lat1)) * Math.sin(rad(lat2)) -
    Math.sin(rad(lat1)) * Math.cos(rad(lat2)) * Math.cos(rad(lon2 - lon1));
  return ((Math.atan2(y, x) * 180) / Math.PI + 360) % 360;
}

const DIRS = ["kuzey", "kuzeydoğu", "doğu", "güneydoğu", "güney", "güneybatı", "batı", "kuzeybatı"];

export function directionName(deg: number): string {
  return DIRS[Math.round(deg / 45) % 8];
}

export function distanceText(m: number): string {
  if (m < 1000) return `${Math.max(1, Math.round(m / 10) * 10)} m`;
  return `${(m / 1000).toFixed(m < 10000 ? 1 : 0).replace(".", ",")} km`;
}
