// Alanlar geometry (src/app/zones.ts): the collar's validation and inside
// test, set capacity, confirmed outside episodes, simplify, GeoJSON.
import assert from "node:assert/strict";
import { test } from "node:test";
import {
  FENCE, areaM2, canonicalE7, checkSet, classifyE7, compileFence, fromGeoJSON, grazing, outsideEpisodes,
  roomFor, simplifyRing, toE7, toGeoJSON, validateRing, zoneAt, type LonLat, type Located, type Zone,
} from "../src/app/zones.ts";

// ~100 m square near the demo station.
const SQ: LonLat[] = [[28.0540, 37.6100], [28.0551, 37.6100], [28.0551, 37.6109], [28.0540, 37.6109]];
const zone = (ring: LonLat[], extra: Partial<Zone> = {}): Zone => ({ id: "a", name: "Mera", ring, active: true, updated: 0, ...extra });

test("the collar's rejection reasons", () => {
  assert.equal(validateRing(SQ), null);
  assert.equal(validateRing(SQ.slice(0, 2)), "tooFew");
  assert.equal(validateRing([...SQ, SQ[0]]), null, "an explicit closing vertex is accepted");
  assert.equal(validateRing([SQ[0], SQ[1], SQ[1], SQ[2]]), "dup");
  assert.equal(validateRing([[28.05, 37.61], [28.06, 37.61], [28.07, 37.61]]), "degenerate");
  assert.equal(validateRing([SQ[0], SQ[2], SQ[1], SQ[3]]), "degenerate", "a symmetric bow tie has zero area (checked first, as on the collar)");
  assert.equal(validateRing([SQ[0], SQ[2], SQ[1], [28.0542, 37.6111]]), "selfX", "a bow tie");
  assert.equal(validateRing([[28, 37], [39, 37], [39, 38]]), "span");
  const many: LonLat[] = Array.from({ length: 65 }, (_, i) => [28.05 + 0.001 * Math.cos(i / 65 * 2 * Math.PI), 37.61 + 0.001 * Math.sin(i / 65 * 2 * Math.PI)]);
  assert.equal(validateRing(many), "tooMany");
});

test("E7 rounding is the collar's", () => {
  assert.deepEqual(toE7([28.05465, 37.61074]), [376107400, 280546500]);
  assert.equal(canonicalE7([...SQ, SQ[0]]).length, 4);
});

test("inside, boundary and outside", () => {
  const v = canonicalE7(SQ);
  assert.equal(classifyE7(v, toE7([28.0545, 37.6104])), "inside");
  assert.equal(classifyE7(v, toE7([28.0540, 37.6104])), "boundary");
  assert.equal(classifyE7(v, toE7([28.0560, 37.6104])), "outside");
  const f = compileFence([zone(SQ)]);
  assert.equal(zoneAt(f, { lat: 37.6104, lon: 28.0540 })?.name, "Mera", "boundary counts as inside");
  assert.equal(zoneAt(f, { lat: 37.6104, lon: 28.0560 }), null);
  assert.equal(compileFence([zone(SQ, { active: false })]).polys.length, 0);
});

test("capacity: 8 areas, 64 vertices in total", () => {
  const nine = Array.from({ length: 9 }, (_, i) => zone(SQ, { id: String(i), name: `M${i}` }));
  assert.equal(checkSet(nine).ok, false);
  assert.equal(checkSet(nine.slice(0, 8)).ok, true);
  assert.equal(checkSet(nine.slice(0, 8)).total, 32);
  assert.equal(roomFor(nine.slice(0, 8), "0", true), FENCE.maxTotalVertices - 28);
  assert.equal(roomFor(nine.slice(0, 8), null, false), FENCE.maxPolygonVertices);
});

test("outside needs 2 of the last 3 records and the newest outside", () => {
  const f = compileFence([zone(SQ)]);
  const IN = { lat: 37.6104, lon: 28.0545 }, OUT = { lat: 37.6104, lon: 28.0600 };
  const at = (i: number, p: { lat: number; lon: number }, hdop: number | null = 1): Located =>
    ({ ...p, time: new Date(Date.UTC(2026, 9, 25, 10, i)).toISOString(), hdop });
  const t = (p: Located) => Date.parse(p.time!);
  assert.equal(outsideEpisodes([at(0, IN), at(1, OUT), at(2, IN)], f, t).length, 0, "one stray fix is not an exit");
  const ep = outsideEpisodes([at(0, IN), at(1, OUT), at(2, OUT), at(3, OUT)], f, t);
  assert.equal(ep.length, 1);
  assert.equal(ep[0].from, Date.UTC(2026, 9, 25, 10, 1), "starts at the first outside record");
  assert.equal(ep[0].to, null);
  const back = outsideEpisodes([at(0, IN), at(1, OUT), at(2, OUT), at(3, IN), at(4, IN)], f, t);
  assert.equal(back[0].to, Date.UTC(2026, 9, 25, 10, 3));
  assert.equal(outsideEpisodes([at(0, IN), at(1, OUT, 9), at(2, OUT, 9)], f, t).length, 0, "poor fixes are not evidence");
  assert.equal(outsideEpisodes([at(0, OUT), at(1, OUT)], compileFence([]), t).length, 0, "no zones: nothing is outside");
});

test("area and simplify", () => {
  const a = areaM2(SQ);
  assert.ok(a > 9000 && a < 10500, `~1 ha, got ${a}`);
  const circle: LonLat[] = Array.from({ length: 60 }, (_, i) => [28.05 + 0.002 * Math.cos(i / 60 * 2 * Math.PI), 37.61 + 0.0016 * Math.sin(i / 60 * 2 * Math.PI)]);
  const s = simplifyRing(circle, 20)!;
  assert.ok(s && s.length <= 20 && s.length >= 3);
  assert.equal(validateRing(s), null);
  assert.ok(Math.abs(areaM2(s) - areaM2(circle)) / areaM2(circle) < 0.05);
});

test("GeoJSON round trip, holes and bad rings skipped", () => {
  let n = 0;
  const back = fromGeoJSON(toGeoJSON([zone(SQ)]), 1, () => `i${n++}`);
  assert.equal(back.zones.length, 1);
  assert.equal(back.zones[0].name, "Mera");
  assert.deepEqual(back.zones[0].ring, SQ);
  const bad = fromGeoJSON(JSON.stringify({ type: "Polygon", coordinates: [[SQ[0], SQ[2], SQ[1], SQ[3], SQ[0]]] }), 1, () => "x");
  assert.equal(bad.zones.length, 0);
  assert.equal(bad.skipped.length, 1);
});

test("free grazing: timed or until turned off", () => {
  assert.equal(grazing({ until: null, forever: false }, 10), false);
  assert.equal(grazing({ until: 20, forever: false }, 10), true);
  assert.equal(grazing({ until: 5, forever: false }, 10), false);
  assert.equal(grazing({ until: null, forever: true }, 10), true);
});

test("outside the zones: status, event, return and serbest otlatma (owner decision 2026-11-13)", async () => {
  const { buildHerd } = await import("../src/app/herd.ts");
  const { deriveEvents } = await import("../src/app/events.ts");
  const NOW = Date.parse("2026-10-25T12:00:00Z");
  const iso = (ms: number) => new Date(ms).toISOString();
  const p = (agoMin: number, lon: number) => ({
    tracker: "c1", seq: agoMin, time: iso(NOW - agoMin * 60_000),
    lat: 37.6104, lon, alt_m: 450, sats: 10, hdop: 1, fix_age_s: 0,
    battery_mv: 4065, battery: "NORMAL", active_s: 30, covered_s: 59,
    source: "GNSS", period_s: 600, rssi: -90, snr: 6,
    received: iso(NOW), delay_s: 0, link: "live" as const,
  });
  const pts = [p(40, 28.0545), p(30, 28.0545), p(20, 28.0600),
               p(10, 28.0601), p(1, 28.0602)];
  const fence = compileFence([zone(SQ)]);
  const base = { points: pts, profiles: {}, assetOf: (d: string) => d, now: NOW, fence };
  const rest = { journal: [], coverage: null,
    station: { id: null, link: "none" as const, phase: "idle" as const, detail: null, held: null, capacity: null, position: null },
    server: { configured: false, url: "", token: "", auto: false, busy: false, uploaded: null,
      pending: null, conflict: 0, rejected: 0, lastOk: null, error: null } };
  // Three records outside, the newest too: outside since the first of them.
  const [a] = buildHerd(base);
  assert.equal(a.status, "out");
  assert.equal(a.outside, true);
  assert.equal(a.outSince, NOW - 20 * 60_000);
  assert.equal(a.grazing, false);
  const out = deriveEvents({ herd: [a], now: NOW, fence, ...rest }).filter((e) => e.kind === "out");
  assert.equal(out.length, 1);
  assert.equal(out[0].at, NOW - 20 * 60_000);
  assert.equal(out[0].end, null);
  assert.equal(out[0].alert, true);

  // Serbest otlatma: still known to be outside, but neither a status nor a warning.
  const [g] = buildHerd({ ...base, grazing: true });
  assert.notEqual(g.status, "out");
  assert.equal(g.outside, true);
  assert.equal(g.grazing, true);
  assert.equal(deriveEvents({ herd: [g], now: NOW, fence, grazing: true, ...rest })
    .filter((e) => e.kind === "out").length, 0);

  // One stray fix outside is nothing.
  const [s] = buildHerd({ ...base, points: [p(30, 28.0545), p(20, 28.0600), p(10, 28.0545), p(1, 28.0545)] });
  assert.equal(s.outside, false);
  assert.notEqual(s.status, "out");

  // Back inside (two records): the period closes and the animal is fine again.
  const back = [...pts.map((x) => ({ ...x, time: iso(Date.parse(x.time) - 30 * 60_000) })),
                p(20, 28.0545), p(10, 28.0545)];
  const [b] = buildHerd({ ...base, points: back });
  assert.equal(b.outside, false);
  assert.notEqual(b.status, "out");
  const closed = deriveEvents({ herd: [b], now: NOW, fence, ...rest }).filter((e) => e.kind === "out");
  assert.equal(closed.length, 1);
  assert.equal(closed[0].end, NOW - 20 * 60_000);

  // A boundary drawn 5 minutes ago (owner's phone 2026-11-13: "15 gündür alan
  // dışında" right after drawing): outside now, but only since the drawing;
  // a period that ended before the drawing is not history of this boundary.
  const drawn = compileFence([zone(SQ, { updated: NOW - 5 * 60_000 })]);
  const [nw] = buildHerd({ ...base, fence: drawn });
  assert.equal(nw.outside, true);
  assert.equal(nw.outSince, NOW - 5 * 60_000);
  const fresh = deriveEvents({ herd: [nw], now: NOW, fence: drawn, ...rest }).filter((e) => e.kind === "out");
  assert.deepEqual(fresh.map((e) => [e.at, e.end]), [[NOW - 5 * 60_000, null]]);
  const [o] = buildHerd({ ...base, points: back, fence: drawn });
  assert.equal(deriveEvents({ herd: [o], now: NOW, fence: drawn, ...rest }).filter((e) => e.kind === "out").length, 0);

  // Interval changed 10 min -> 60 min: the first hourly report is on time, not silence.
  const slow = [p(130, 28.0545), p(120, 28.0545), { ...p(60, 28.0545), period_s: 3600 }, { ...p(1, 28.0545), period_s: 3600 }];
  const [q] = buildHerd({ ...base, points: slow });
  assert.equal(deriveEvents({ herd: [q], now: NOW, fence, ...rest }).filter((e) => e.kind === "silent" || e.kind === "lost").length, 0);
  // The collar confirmed 60 min but its newest record still says 10 min: 45 minutes without news is not silence.
  const waiting = [p(65, 28.0545), p(55, 28.0545), p(45, 28.0545)];
  const [w0] = buildHerd({ ...base, points: waiting });
  assert.equal(deriveEvents({ herd: [w0], now: NOW, fence, ...rest }).filter((e) => e.kind === "silent").length, 1);
  const [stale] = buildHerd({ ...base, points: waiting, intervals: { C1: { s: 3600, at: NOW - 600 * 60_000 } } });
  // An older confirmation is the base interval: the collar may be on base / 3
  // outside its zones, so silence waits 1.25 x base (3 x 0.42 x 3600 s = 75 min), not 3 x 10 min.
  assert.equal(stale.periodS, 1512);
  // KQ-03: after 45 minutes the location is no longer fresh (25.2 min
  // effective period), but the collar is not yet silent (75.6 min limit).
  assert.equal(stale.freshness, "aging");
  assert.equal(stale.status, "aging");
  assert.equal(deriveEvents({ herd: [stale], now: NOW, fence, ...rest })
    .filter((e) => e.kind === "silent" || e.kind === "lost").length, 0);
  const [late] = buildHerd({ ...base, now: NOW + 35 * 60_000, points: waiting, intervals: { C1: { s: 3600, at: NOW - 600 * 60_000 } } });
  assert.equal(late.status, "silent", "80 dakika sonra sessiz");
  const [w1] = buildHerd({ ...base, points: waiting, intervals: { C1: { s: 3600, at: NOW - 44 * 60_000 } } });
  assert.equal(w1.status, "ok");
  assert.equal(deriveEvents({ herd: [w1], now: NOW, fence, ...rest }).filter((e) => e.kind === "silent" || e.kind === "lost").length, 0);
  // A zone removed a minute ago: the set changed, so the animal is outside only since then.
  const [rm] = buildHerd({ ...base, fence: compileFence([zone(SQ)], NOW - 60_000) });
  assert.equal(rm.outSince, NOW - 60_000);

  // Outside and then no news for three reports (period 600 s: 30 min): may be lost.
  const gone = pts.map((x) => ({ ...x, time: iso(Date.parse(x.time) - 40 * 60_000) }));
  const [l] = buildHerd({ ...base, points: gone });
  assert.equal(l.outside, true);
  assert.equal(l.status, "lost");
  // The same silence inside the zone is only "no news".
  const inside = [p(80, 28.0545), p(70, 28.0545), p(60, 28.0545), p(50, 28.0545), p(41, 28.0545)];
  assert.equal(buildHerd({ ...base, points: inside })[0].status, "silent");

  // No zones defined: never outside.
  const [n] = buildHerd({ ...base, fence: compileFence([]) });
  assert.equal(n.outside, false);
});

test("GeoJSON polygon holes cannot silently become allowed grazing land", () => {
  const hole: LonLat[] = [[28.0542, 37.6102], [28.0544, 37.6102],
    [28.0544, 37.6104], [28.0542, 37.6104]];
  const coordinates = [[...SQ, SQ[0]], [...hole, hole[0]]];
  const r = fromGeoJSON(JSON.stringify({ type: "Polygon", coordinates }), 1, () => "x");
  assert.equal(r.zones.length, 0);
  assert.match(r.skipped.join(" "), /boşluk/);
  assert.throws(() => fromGeoJSON("null", 1, () => "x"), /GeoJSON/);
});
