// Map data from the phone ledger. Port of points_from_db + classify_links
// (tools/orun_edge_bench.py); same JSON, so tools/edge_map/index.html works
// unchanged. Rules kept for the app (SF5L4): "where is it now" is the newest
// RECORD time, never the latest arrival; late arrival alone is not proof of
// no radio link.
import { fromHex } from "./bytes.ts";
import type { Db } from "./db.ts";
import { describePeriodic, isoSeconds } from "./product.ts";

export const LIVE_S = 30;   // reached the Edge this soon after capture: sent from (about) there
export const CUT_S = 180;   // later than this: queued somewhere, or recorded with no link
export const NEAR_S = 150;  // a live arrival this close in capture time proves the link was up

export interface Point {
  tracker: string; seq: number; time: string | null; lat: number; lon: number;
  sats: number | null; hdop: number | null; fix_age_s: number | null;
  battery_mv: number | null; battery: string | null; active_s: number | null;
  covered_s: number | null; source: string; period_s: number;
  rssi: number | null; snr: number | null; received: string | null;
  delay_s: number | null; link: "live" | "through" | "cut" | null;
}
export interface PointsResult { count: number; skipped: number; now: string; points: Point[]; }

const epoch = (iso: string | null) => (iso ? Date.parse(iso) / 1000 : null);

export function classifyLinks(points: Point[], sessions: [number, number][]): void {
  const live = new Map<string, number[]>();
  for (const p of points) {
    const t = epoch(p.time);
    if (t !== null && p.rssi !== null && p.delay_s !== null && p.delay_s <= LIVE_S) {
      if (!live.has(p.tracker)) live.set(p.tracker, []);
      live.get(p.tracker)!.push(t);
    }
  }
  for (const times of live.values()) times.sort((a, b) => a - b);
  for (const p of points) {
    p.link = null;
    const t = epoch(p.time);
    if (t === null || p.received === null || p.delay_s === null) continue;
    if (p.rssi !== null && p.delay_s <= LIVE_S) { p.link = "live"; continue; }
    if (!sessions.some(([a, b]) => a <= t && t <= b)) continue;   // nobody listened
    if (p.delay_s <= CUT_S) { p.link = "through"; continue; }
    const times = live.get(p.tracker) ?? [];
    let lo = 0, hi = times.length;                                  // bisect_left
    while (lo < hi) { const mid = (lo + hi) >> 1; if (times[mid] < t - NEAR_S) lo = mid + 1; else hi = mid; }
    if (!(lo < times.length && times[lo] <= t + NEAR_S)) p.link = "cut";
  }
}

export async function buildPoints(db: Db, nowSeconds: number): Promise<PointsResult> {
  const rows = await db.query(
    "SELECT o.device_id,o.family,o.plaintext,r.first_seen_utc,r.gw_rssi,r.gw_snr " +
    "FROM observations o LEFT JOIN receipts r USING (device_id,incarnation,sequence) " +
    "ORDER BY o.device_id,o.incarnation,o.sequence");
  const sessions = (await db.query("SELECT started_utc,alive_utc FROM sessions"))
    .map((s) => [Number(s.started_utc), Number(s.alive_utc)] as [number, number]);
  const points: Point[] = [];
  let skipped = 0;
  for (const r of rows) {
    if (Number(r.family) !== 1) continue;
    const d = describePeriodic(fromHex(String(r.device_id)), fromHex(String(r.plaintext)));
    if (d.lat === null || d.lon === null) { skipped++; continue; }
    const seen = r.first_seen_utc === null ? null : Number(r.first_seen_utc);
    const recorded = epoch(d.time);
    points.push({
      tracker: d.tracker, seq: d.sequence, time: d.time, lat: d.lat, lon: d.lon,
      sats: d.sats, hdop: d.hdop, fix_age_s: d.fix_age_s, battery_mv: d.battery_mv,
      battery: d.battery, active_s: d.active_s, covered_s: d.covered_s, source: d.source,
      period_s: d.period_s,
      rssi: r.gw_rssi === null || r.gw_rssi === undefined ? null : Number(r.gw_rssi),
      snr: r.gw_snr === null || r.gw_snr === undefined ? null : Number(r.gw_snr),
      received: seen ? isoSeconds(seen) : null,
      delay_s: seen && recorded !== null ? Math.max(0, Math.floor(seen - recorded)) : null,
      link: null,
    });
  }
  classifyLinks(points, sessions);
  return { count: points.length, skipped, now: new Date(nowSeconds * 1000).toISOString(), points };
}
