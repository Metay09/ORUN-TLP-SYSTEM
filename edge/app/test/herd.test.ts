// Herd view model (src/app/herd.ts) and the Turkish wording (i18n/tr.ts):
// freshness from RECORD time, attention-first order, battery over the last
// day, distance/direction.
import assert from "node:assert/strict";
import { test } from "node:test";
import type { Point } from "../src/core/points.ts";
import { bearingDeg, distanceM } from "../src/app/geo.ts";
import { batteryOf, buildHerd, freshLimit, freshnessOf, freshRatio, herdSummary, matches } from "../src/app/herd.ts";
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

test("freshness follows the collar's actual period, not old 10min/2T floors", () => {
  const minute = 60_000;
  // One-minute report: one or two minutes is fresh; three is aging.
  assert.equal(freshLimit(minute), 2 * minute);
  assert.equal(freshnessOf(NOW - 2 * minute, 60, NOW), "fresh");
  assert.equal(freshnessOf(NOW - 2 * minute - 1, 60, NOW), "aging");
  assert.equal(freshnessOf(NOW - 5 * minute, 59, NOW), "aging");
  assert.equal(freshnessOf(NOW - 30 * minute, 59, NOW), "aging");
  assert.equal(freshnessOf(NOW - 2 * 3600_000, 59, NOW), "stale");

  // A 15-minute collar is not fresh for an additional 15 minutes.
  assert.equal(freshLimit(15 * minute), 15 * minute);
  assert.equal(freshnessOf(NOW - 15 * minute, 900, NOW), "fresh");
  assert.equal(freshnessOf(NOW - 15 * minute - 1, 900, NOW), "aging");
  assert.equal(freshnessOf(NOW - 25 * minute, 900, NOW), "aging");

  // A one-hour collar stays fresh for its report period, not two hours.
  assert.equal(freshLimit(60 * minute), 60 * minute);
  assert.equal(freshnessOf(NOW - 60 * minute, 3600, NOW), "fresh");
  assert.equal(freshnessOf(NOW - 60 * minute - 1, 3600, NOW), "aging");
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
    pt("00000000000000BB", 60, { received: iso(NOW) }),              // arrived now, measured 1 h ago
    pt("00000000000000CC", 2, { battery: "LOW", battery_mv: 3600 }),
    pt("00000000000000EE", 5 * 60),                                   // 5 h without news: may be lost
  ];
  const herd = herdOf(points, { "00000000000000AA": named, "00000000000000DD": blankProfile("00000000000000DD") });
  assert.deepEqual(herd.map((a) => a.id.slice(-2)), ["EE", "BB", "CC", "AA", "DD"]);
  const [ee, bb, cc, aa, dd] = herd;
  assert.equal(ee.status, "lost");
  assert.equal(ee.seen, "5 saattir haber yok · kayıp olabilir");
  assert.equal(bb.status, "silent");
  assert.equal(bb.seen, "1 saattir haber yok");                       // late arrival is not "seen now"
  assert.equal(cc.battery, "low");
  assert.equal(aa.name, "Sarıkız");
  assert.equal(aa.initials, "S");
  assert.equal(aa.seen, "1 dk önce görüldü");
  assert.equal(aa.points.length, 2);
  assert.equal(dd.name, "ORUN-000000DD");                              // adsız: Bluetooth adı
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

test("Pil 2: one percent table with the firmware; FULL is 100 %, no battery has none", async () => {
  const { batteryPercent, readingPercent, externallyPowered } = await import("../src/app/herd.ts");
  // Same values as firmware/tests/m3/test_battery_policy.cpp percentEstimate().
  for (const [mv, p] of [[4200, 100], [4190, 100], [4120, 95], [4060, 91], [4050, 90], [3990, 80],
                         [3400, 0], [3840, 64], [4020, 85], [3500, 24], [3350, 0]])
    assert.equal(batteryPercent(mv), p, `${mv} mV`);
  // On its own battery a just-unplugged cell no longer reads 100 % for hours.
  assert.equal(readingPercent({ mv: 4065, power: "BATTERY" }), 91);
  assert.equal(readingPercent({ mv: 4010, power: "FULL" }), 100);
  assert.equal(readingPercent({ mv: 4300, power: "NO_BATTERY" }), null);
  assert.equal(readingPercent({ mv: 3840, power: null }), 64);
  assert.equal(externallyPowered({ usb: true, power: "CHARGING" }), true);
  assert.equal(externallyPowered({ usb: false, power: "CHARGING" }), false);   // solar: the battery's own balance
  assert.equal(batteryOf(pt("A", 1, { battery_mv: 3990, power: "FULL", usb: true })).percent, 100);
  assert.deepEqual(batteryOf(pt("A", 1, { battery_mv: 4300, power: "NO_BATTERY", usb: true })), { level: "unknown", percent: null });
  // Charging from low is not an alarm.
  assert.equal(batteryOf(pt("A", 1, { battery_mv: 3450, battery: "LOW", power: "CHARGING", usb: true })).level, "good");
});

test("Pil 2: the record's byte 42 carries the power state and USB", async () => {
  const { describePeriodic, periodicValid } = await import("../src/core/product.ts");
  const rec = (b42: number) => {
    const p = new Uint8Array(64);
    p[0] = 1; p[1] = 32; p[3] = 1;
    p[11] = 1; p[15] = 1; p[23] = 60; p[38] = 0xff; p[39] = 0xff;
    p[42] = b42; p[56] = 0x10; p[57] = 0x04;   // 4100 mV
    return p;
  };
  const dev = new Uint8Array(8);
  assert.ok(periodicValid(rec(1)));
  assert.ok(periodicValid(rec(1 | 3 << 2 | 0x20)));
  assert.ok(!periodicValid(rec(1 | 5 << 2)));
  assert.ok(!periodicValid(rec(1 | 0x40)));
  const full = describePeriodic(dev, rec(1 | 3 << 2 | 0x20));
  assert.equal(full.power, "FULL");
  assert.equal(full.usb, true);
  const old = describePeriodic(dev, rec(1));
  assert.equal(old.power, null);
  assert.equal(old.usb, null);
});
