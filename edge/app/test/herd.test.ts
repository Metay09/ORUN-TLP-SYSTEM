// Herd view model (src/app/herd.ts) and the Turkish wording (i18n/tr.ts):
// freshness from RECORD time, attention-first order, battery over the last
// day, distance/direction.
import assert from "node:assert/strict";
import { test } from "node:test";
import type { Point } from "../src/core/points.ts";
import { bearingDeg, distanceM } from "../src/app/geo.ts";
import { batteryOf, buildHerd, freshnessOf, freshRatio, herdSummary, matches } from "../src/app/herd.ts";
import { fmt } from "../src/app/i18n/tr.ts";
import { AssetStore, blankProfile } from "../src/app/stores.ts";
import type { AssetProfile } from "../src/app/model.ts";

const NOW = Date.parse("2026-10-25T12:00:00Z");
const iso = (ms: number) => new Date(ms).toISOString().replace(".000Z", "Z");

export function pt(tracker: string, agoMin: number, extra: Partial<Point> = {}): Point {
  return {
    tracker, seq: 1, time: iso(NOW - agoMin * 60_000), lat: 37.61, lon: 28.05, alt_m: 450,
    sats: 10, hdop: 1, fix_age_s: 0, battery_mv: 4065, battery: "NORMAL",
    active_s: 55, covered_s: 59, source: "GNSS", period_s: 59,
    rssi: -90, snr: 6, received: iso(NOW), delay_s: 0, link: "live", ...extra,
  };
}

const herdOf = (points: Point[], profiles: Record<string, AssetProfile> = {}) =>
  buildHerd({ points, profiles, assetOf: (d) => d, now: NOW });

test("freshness follows the collar period, with floors", () => {
  assert.equal(freshnessOf(NOW - 5 * 60_000, 59, NOW), "fresh");
  assert.equal(freshnessOf(NOW - 30 * 60_000, 59, NOW), "aging");
  assert.equal(freshnessOf(NOW - 2 * 3600_000, 59, NOW), "stale");
  assert.equal(freshnessOf(NOW - 25 * 60_000, 900, NOW), "fresh");   // 15 min period: 2T = 30 min
  assert.equal(freshnessOf(null, 59, NOW), "none");
  assert.equal(freshRatio(NOW, 59, NOW), 1);
  assert.equal(freshRatio(NOW - 2 * 3600_000, 59, NOW), 0);
});

test("Turkish relative time, durations, distances", () => {
  assert.equal(fmt.ago(NOW - 20_000, NOW), "az önce");
  assert.equal(fmt.ago(NOW - 8 * 60_000, NOW), "8 dk önce");
  assert.equal(fmt.ago(NOW - 3 * 3600_000, NOW), "3 sa önce");
  assert.equal(fmt.since(NOW - 40 * 60_000, NOW), "40 dakikadır");
  assert.equal(fmt.since(NOW - 2 * 3600_000, NOW), "2 saattir");
  assert.equal(fmt.since(NOW - 3 * 86400_000, NOW), "3 gündür");
  assert.equal(fmt.duration(160 * 60_000), "2 sa 40 dk");
  assert.equal(fmt.duration(35 * 60_000), "35 dk");
  assert.equal(fmt.distance(400), "400 m");
  assert.equal(fmt.distance(1530), "1,5 km");
  assert.deepEqual(fmt.distanceParts(423), ["425", "m"]);
  assert.equal(fmt.period(900), "15 dk");
});

test("battery level", () => {
  assert.equal(batteryOf(pt("A", 1)).level, "good");
  assert.equal(batteryOf(pt("A", 1, { battery: "LOW", battery_mv: 3600 })).level, "low");
  assert.equal(batteryOf(pt("A", 1, { battery: "CRITICAL" })).level, "critical");
  assert.equal(batteryOf(pt("A", 1, { battery_mv: null, battery: null })).level, "unknown");
});

test("herd: newest RECORD per collar, attention first, plain sentences", () => {
  const named: AssetProfile = { ...blankProfile("00000000000000AA"), name: "Sarıkız", tag: "TR 09 1234" };
  const points = [
    pt("00000000000000AA", 3), pt("00000000000000AA", 1),
    pt("00000000000000BB", 120, { received: iso(NOW) }),             // arrived now, measured 2 h ago
    pt("00000000000000CC", 2, { battery: "LOW", battery_mv: 3600 }),
    pt("00000000000000EE", 5 * 60),                                   // 5 h without news: may be lost
  ];
  const herd = herdOf(points, { "00000000000000AA": named, "00000000000000DD": blankProfile("00000000000000DD") });
  assert.deepEqual(herd.map((a) => a.id.slice(-2)), ["EE", "BB", "CC", "AA", "DD"]);
  const [ee, bb, cc, aa, dd] = herd;
  assert.equal(ee.status, "lost");
  assert.equal(ee.seen, "5 saattir haber yok · kayıp olabilir");
  assert.equal(bb.status, "silent");
  assert.equal(bb.seen, "2 saattir haber yok");                       // late arrival is not "seen now"
  assert.equal(cc.battery, "low");
  assert.equal(aa.name, "Sarıkız");
  assert.equal(aa.initials, "S");
  assert.equal(aa.seen, "1 dk önce görüldü");
  assert.equal(aa.points.length, 2);
  assert.equal(dd.name, "Tasma 00DD");
  assert.equal(dd.status, "waiting");
  assert.equal(herdSummary(herd), "5 hayvan · 3 tanesine bakın");
  assert.equal(herdSummary([aa]), "1 hayvan · hepsi yakın zamanda görüldü");
  assert.equal(herdSummary([]), "Henüz hayvan yok");
  assert.ok(matches(aa, "sarı") && matches(aa, "1234") && matches(aa, "00aa") && !matches(aa, "kara"));
});

test("battery status is the worst of the last day (solar collars dip at night)", () => {
  const herd = herdOf([pt("A", 600, { battery: "LOW", battery_mv: 3600 }), pt("A", 1)]);
  assert.equal(herd[0].battery, "low");
  assert.equal(herd[0].status, "battery-low");
  const later = herdOf([pt("A", 26 * 60, { battery: "LOW", battery_mv: 3600 }), pt("A", 1)]);
  assert.equal(later[0].battery, "good");
});

test("assignment with validity: a collar moved to another animal keeps old records on the old one", () => {
  const store = new AssetStore(null, {
    assets: { cow1: { ...blankProfile("cow1"), name: "Eski" }, cow2: { ...blankProfile("cow2"), name: "Yeni" } },
    assignments: [{ device: "C0", asset: "cow1", from: 0 }, { device: "C0", asset: "cow2", from: NOW - 60 * 60_000 }],
  });
  const herd = buildHerd({ points: [pt("C0", 120), pt("C0", 10)], profiles: store.get().assets, assetOf: store.resolver(), now: NOW });
  const by = Object.fromEntries(herd.map((a) => [a.name, a.points.length]));
  assert.deepEqual(by, { Eski: 1, Yeni: 1 });
});

test("distance and direction", () => {
  const d = distanceM({ lat: 37.6, lon: 28.05 }, { lat: 37.6036, lon: 28.05 });
  assert.ok(Math.abs(d - 400) < 5, String(d));
  assert.equal(fmt.direction(bearingDeg({ lat: 37.6, lon: 28.05 }, { lat: 37.61, lon: 28.06 })), "kuzeydoğu");
  assert.equal(fmt.direction(bearingDeg({ lat: 37.6, lon: 28.05 }, { lat: 37.59, lon: 28.05 })), "güney");
});
