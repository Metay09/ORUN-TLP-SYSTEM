// Herd view model (src/ui/herd.ts): freshness from RECORD time, Turkish
// wording, battery, attention-first order, distance/direction.
import assert from "node:assert/strict";
import { test } from "node:test";
import type { Point } from "../src/core/points.ts";
import {
  ago, batteryOf, bearingDeg, buildHerd, directionName, distanceM, distanceText,
  freshnessOf, herdSummary, since,
} from "../src/ui/herd.ts";

const NOW = Date.parse("2026-10-25T12:00:00Z");
const iso = (ms: number) => new Date(ms).toISOString().replace(".000Z", "Z");

function pt(tracker: string, agoMin: number, extra: Partial<Point> = {}): Point {
  return {
    tracker, seq: 1, time: iso(NOW - agoMin * 60_000), lat: 37.61, lon: 28.05,
    sats: 10, hdop: 1, fix_age_s: 0, battery_mv: 4065, battery: "NORMAL",
    active_s: 55, covered_s: 59, source: "GNSS", period_s: 59,
    rssi: -90, snr: 6, received: iso(NOW), delay_s: 0, link: "live", ...extra,
  };
}

test("freshness follows the collar period, with floors", () => {
  assert.equal(freshnessOf(NOW - 5 * 60_000, 59, NOW), "fresh");
  assert.equal(freshnessOf(NOW - 30 * 60_000, 59, NOW), "aging");
  assert.equal(freshnessOf(NOW - 2 * 3600_000, 59, NOW), "stale");
  assert.equal(freshnessOf(NOW - 25 * 60_000, 900, NOW), "fresh");   // 15 min period: 2T = 30 min
  assert.equal(freshnessOf(null, 59, NOW), "none");
});

test("Turkish relative time and durations", () => {
  assert.equal(ago(NOW - 20_000, NOW), "az önce");
  assert.equal(ago(NOW - 8 * 60_000, NOW), "8 dk önce");
  assert.equal(ago(NOW - 3 * 3600_000, NOW), "3 sa önce");
  assert.equal(since(NOW - 40 * 60_000, NOW), "40 dakikadır");
  assert.equal(since(NOW - 2 * 3600_000, NOW), "2 saattir");
  assert.equal(since(NOW - 3 * 86400_000, NOW), "3 gündür");
});

test("battery wording", () => {
  assert.equal(batteryOf(pt("A", 1)).level, "good");
  assert.match(batteryOf(pt("A", 1)).text!, /^Pil iyi · %8\d$/);
  assert.equal(batteryOf(pt("A", 1, { battery: "LOW", battery_mv: 3600 })).level, "low");
  assert.equal(batteryOf(pt("A", 1, { battery: "CRITICAL" })).level, "critical");
  assert.equal(batteryOf(pt("A", 1, { battery_mv: null, battery: null })).level, "unknown");
});

test("herd: newest RECORD per collar, attention first, plain sentences, 24 h trail", () => {
  const points = [
    pt("00000000000000AA", 3), pt("00000000000000AA", 1),
    pt("00000000000000BB", 120, { received: iso(NOW) }),           // arrived now, measured 2 h ago
    pt("00000000000000CC", 2, { battery: "LOW", battery_mv: 3600 }),
    pt("00000000000000AA", 26 * 60),                                // older than 24 h before the newest
  ];
  const herd = buildHerd(points, { "00000000000000AA": { name: "Sarıkız" }, "00000000000000DD": {} }, NOW);
  assert.deepEqual(herd.map((a) => a.id.slice(-2)), ["BB", "CC", "DD", "AA"]);   // waiting for a first fix ranks above fine
  const [bb, cc, dd, aa] = herd;
  assert.equal(bb.freshness, "stale");
  assert.equal(bb.seen, "2 saattir haber yok");                     // late arrival is not "seen now"
  assert.equal(cc.battery, "low");
  assert.equal(aa.name, "Sarıkız");
  assert.equal(aa.seen, "1 dk önce görüldü");
  assert.equal(aa.movement, "hareketli");
  assert.equal(aa.trail.length, 2);
  assert.equal(dd.name, "Tasma 00DD");
  assert.equal(dd.seen, "İlk konum bekleniyor");
  assert.equal(herdSummary(herd), "4 hayvan · 2 tanesine bakın");
  assert.equal(herdSummary([aa]), "1 hayvan · hepsi yakın zamanda görüldü");
  assert.equal(aa.named, true);
  assert.equal(dd.named, false);
  assert.equal(herdSummary([]), "Henüz hayvan yok");
});

test("distance and direction", () => {
  const d = distanceM(37.6, 28.05, 37.6036, 28.05);
  assert.ok(Math.abs(d - 400) < 5, String(d));
  assert.equal(distanceText(d), "400 m");
  assert.equal(distanceText(1530), "1,5 km");
  assert.equal(directionName(bearingDeg(37.6, 28.05, 37.61, 28.06)), "kuzeydoğu");
  assert.equal(directionName(bearingDeg(37.6, 28.05, 37.59, 28.05)), "güney");
});
