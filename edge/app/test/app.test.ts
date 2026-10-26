// App data layer: events from record gaps / battery / stillness, 5 m
// coverage from live records only, per-day summaries, and the sample herd.
import assert from "node:assert/strict";
import { test } from "node:test";
import type { Point } from "../src/core/points.ts";
import { daySummaries, hourlyActivity } from "../src/app/activity.ts";
import { classify, coverageAt, coverageCells, coverageSummary } from "../src/app/coverage.ts";
import { demoBook, demoPoints } from "../src/app/demo.ts";
import { deriveEvents, isActive } from "../src/app/events.ts";
import { buildHerd } from "../src/app/herd.ts";
import type { ServerView, Station } from "../src/app/model.ts";

const NOW = Date.parse("2026-10-25T12:00:00Z");
const MIN = 60_000;
const iso = (ms: number) => new Date(ms).toISOString().replace(".000Z", "Z");

function rec(tracker: string, agoMin: number, extra: Partial<Point> = {}): Point {
  return {
    tracker, seq: 1, time: iso(NOW - agoMin * MIN), lat: 37.61, lon: 28.05, alt_m: null,
    sats: 10, hdop: 1, fix_age_s: 0, battery_mv: 4065, battery: "NORMAL",
    active_s: 300, covered_s: 900, source: "GNSS", period_s: 900,
    rssi: -95, snr: 4, received: iso(NOW - agoMin * MIN + 3000), delay_s: 3, link: "live", ...extra,
  };
}
const station: Station = { id: "GW", link: "ble", phase: "connected", detail: null, held: 10, capacity: 1056, position: null };
const server: ServerView = { configured: true, url: "u", token: "t", auto: true, busy: false, uploaded: 1, pending: 0, conflict: 0, rejected: 0, lastOk: null, error: null };
const herdOf = (points: Point[]) => buildHerd({ points, profiles: {}, assetOf: (d) => d, now: NOW });
const events = (points: Point[], extra: Partial<Parameters<typeof deriveEvents>[0]> = {}) =>
  deriveEvents({ herd: herdOf(points), station, server, journal: [], coverage: null, now: NOW, ...extra });

test("a gap in measurement time is a resolved silent event; the open gap is active", () => {
  const pts = [rec("A", 600), rec("A", 585), rec("A", 450), rec("A", 435), rec("A", 200)];
  const ev = events(pts);
  const silent = ev.filter((e) => e.kind === "silent" || e.kind === "lost");
  assert.equal(silent.length, 3, JSON.stringify(silent.map((e) => [e.kind, e.end])));
  const active = silent.filter(isActive);
  assert.equal(active.length, 1);
  assert.equal(active[0].kind, "lost");                        // 200 min > max(12T, 3 h)
  assert.ok(silent.some((e) => e.kind === "silent" && e.end !== null));   // 135 min gap: silent, resolved
});

test("late arrival fills the gap: no event when the records exist in measurement time", () => {
  const pts = [0, 15, 30, 45, 60, 75].map((m) => rec("A", m, { received: iso(NOW), delay_s: m * 60, link: "cut" }));
  assert.equal(events(pts).filter((e) => e.alert).length, 0);
});

test("battery: one episode with the worst level, closed only after a good day", () => {
  const pts: Point[] = [];
  for (let m = 3000; m >= 0; m -= 60) {
    const night = (Math.floor(m / 60) % 24) < 10;
    pts.push(rec("B", m, m > 1500 ? (night ? { battery: "LOW", battery_mv: 3580 } : {}) : {}));
  }
  const bat = events(pts).filter((e) => e.kind.startsWith("battery"));
  assert.equal(bat.length, 1);
  assert.equal(bat[0].kind, "battery-low");
  assert.notEqual(bat[0].end, null);
  const crit = events([rec("C", 60, { battery: "LOW", battery_mv: 3600 }), rec("C", 30, { battery: "CRITICAL", battery_mv: 3520 }), rec("C", 0)]);
  const c = crit.filter((e) => e.kind.startsWith("battery"));
  assert.equal(c.length, 1);
  assert.equal(c[0].kind, "battery-critical");
  assert.equal(c[0].end, null);
});

test("stillness of 3 h or more", () => {
  const pts = [240, 225, 210, 195, 180, 165, 150, 135, 120, 105, 90, 75, 60, 45, 30, 15]
    .map((m) => rec("D", m, { active_s: m <= 210 && m >= 15 ? 0 : 300 }));
  pts.push(rec("D", 0));
  const still = events(pts).filter((e) => e.kind === "still");
  assert.equal(still.length, 1);
  assert.notEqual(still[0].end, null);
});

test("station memory and server problems are live conditions with stable ids", () => {
  const ev = events([rec("A", 1)], {
    station: { ...station, held: 900 },
    server: { ...server, error: "ağ yok", pending: 12 },
  });
  assert.ok(ev.some((e) => e.id === "station-full:GW" && isActive(e)));
  assert.ok(ev.some((e) => e.id === "server-down" && isActive(e)));
});

test("coverage: 5 m cells from live records; cut-only cells say 'no link'; replays do not count", () => {
  const pts = [
    rec("A", 30, { lat: 37.61, lon: 28.05, rssi: -90, snr: 6 }),
    rec("A", 20, { lat: 37.61001, lon: 28.05001, rssi: -100, snr: -4 }),          // same 5 m cell
    rec("A", 10, { lat: 37.6110, lon: 28.0510, link: "cut", rssi: -80, snr: 9 }),  // heard later, elsewhere
    rec("A", 5, { lat: 37.6120, lon: 28.0520, link: "through", rssi: -80 }),       // not a live sample
  ];
  const cov = coverageCells(pts);
  assert.equal(cov.cells.length, 2);
  const live = cov.cells.find((c) => c.n === 2)!;
  assert.equal(live.snr, 1);
  assert.equal(live.cls, "strong");
  assert.equal(cov.cells.find((c) => c.n === 0)!.cls, "none");
  assert.equal(coverageAt(cov, 37.61, 28.05), "strong");
  assert.equal(coverageAt(cov, 37.6200, 28.0600), null);
  assert.deepEqual(coverageSummary(cov.cells), { cells: 2, strong: 50, weak: 50 });
  assert.equal(classify(-125, null), "weak");
  assert.equal(classify(null, -12), "weak");
});

test("activity: hourly shares and per-day summaries", () => {
  const pts = [rec("A", 30, { active_s: 450 }), rec("A", 15, { active_s: 900 }), rec("A", 26 * 60)];
  const h = hourlyActivity(pts, NOW);
  assert.equal(h.length, 24);
  assert.equal(h[23], 0.75);
  assert.equal(h[0], null);
  const days = daySummaries(pts);
  assert.equal(days.length, 2);
  assert.equal(days[0].records, 2);
  assert.equal(days[0].activeMs, 1350 * 1000);
});

test("sample herd: deterministic, stable history, the story's states", () => {
  const now = Date.parse("2026-10-26T10:07:00Z");
  const a = demoPoints(now), b = demoPoints(now + 15 * 60_000, now);
  assert.ok(a.length > 5000);
  const key = (p: Point) => `${p.tracker}:${p.seq}`;
  const later = new Map(b.map((p) => [key(p), p]));
  const old = a.filter((p) => p.link === "live" && Date.parse(p.time!) > now + 15 * 60_000 - 7 * 86400_000);
  assert.ok(old.every((p) => later.get(key(p))?.lat === p.lat), "history must not move");
  const newest = (l: Point[]) => l.reduce((x, p) => Math.max(x, Date.parse(p.time!)), 0);
  assert.ok(newest(b) > newest(a), "new records arrive");
  const book = demoBook(now);
  const herd = buildHerd({ points: a, profiles: book.assets, assetOf: (d) => d, now });
  const st = Object.fromEntries(herd.map((x) => [x.name, x.status]));
  assert.equal(st["Duman"], "lost");
  assert.equal(st["Benekli"], "silent");
  assert.equal(st["Yıldız"], "aging");
  assert.equal(st["Pamuk"], "battery-critical");
  assert.equal(st["Karabaş"], "battery-low");
  assert.equal(st["Sarıkız"], "ok");
  assert.ok(a.some((p) => p.link === "cut"), "some records come back from behind the ridge later");
  assert.ok(a.every((p) => p.alt_m !== null && p.time !== null));
});
