// Map display geometry (owner decision 2026-11-16): one solid line through
// every fix, no dashes, gaps or jump filtering. Original observations are
// never changed.
import assert from "node:assert/strict";
import { test } from "node:test";
import type { Observation } from "../src/app/model.ts";
import { readableTrail, trailAge } from "../src/features/map/routeGeometry.ts";
import {
  addDays, fromIsoDate, isoDate, pickFromWindow, pickWindow, setHours, startOfDay, wholeDay,
} from "../src/features/map/trailWindow.ts";

const point = (seconds: number, lat: number, lon: number, seq = seconds): Observation => ({
  tracker: "A", seq, time: new Date(Date.UTC(2026, 10, 10) + seconds * 1000).toISOString(),
  lat, lon, period_s: 60,
} as Observation);

test("map trail: a long silence is still joined, not broken or dashed", () => {
  const p = [point(0, 37, 28), point(60, 37.0003, 28), point(7200, 37.0006, 28.0003)];
  const s = readableTrail(p);
  assert.equal(s.fixes.length, 3);
  assert.equal(s.line.length, 3);
});

test("map trail: a GPS jump is drawn like any other fix", () => {
  const p = [point(0, 37, 28), point(60, 37.02, 28), point(120, 37.0001, 28)];
  const s = readableTrail(p);
  assert.deepEqual(s.fixes.map((x) => x.seq), [0, 60, 120]);
  assert.equal(s.line.length, 3);
});

test("map trail: a real fast move (e.g. a trailer ride) stays in the line", () => {
  const p = [point(0, 37, 28), point(60, 37.0001, 28), point(120, 37.02, 28), point(180, 37.0201, 28)];
  const s = readableTrail(p);
  assert.equal(s.fixes.length, 4);
});

test("map trail: a lone fix is kept so it can be drawn as a dot", () => {
  const s = readableTrail([point(0, 37, 28)]);
  assert.equal(s.fixes.length, 1);
  assert.equal(s.line.length, 1);
});

test("map trail: straight route drops intermediate line vertices but keeps every dot", () => {
  const p = Array.from({ length: 20 }, (_, i) => point(i * 60, 37, 28 + i * 0.0001));
  const s = readableTrail(p);
  assert.equal(s.line.length, 2);
  assert.equal(s.line[0], p[0]);
  assert.equal(s.line[1], p[p.length - 1]);
  assert.equal(s.fixes.length, 20);
});

test("map trail: repeated timestamp is not an extra segment", () => {
  const p = [point(0, 37, 28), point(0, 37.001, 28, 200), point(60, 37.0001, 28)];
  const s = readableTrail(p);
  assert.equal(s.fixes.length, 2);
});

test("map trail: a real corner remains visible instead of being straightened", () => {
  const p = [point(0, 37, 28), point(60, 37.0003, 28), point(120, 37.0003, 28.0003)];
  assert.equal(readableTrail(p).line.length, 3);
});

test("map trail: age runs from faint at the period start to strong at its end", () => {
  assert.equal(trailAge(0, 0, 100), 0);
  assert.equal(trailAge(50, 0, 100), 0.5);
  assert.equal(trailAge(150, 0, 100), 1);
  assert.equal(trailAge(-5, 0, 100), 0);
  assert.equal(trailAge(5, 10, 10), 1);
});

test("trail period: last 2 hours, a whole day, and hours of a day", () => {
  const now = new Date(2026, 10, 16, 14, 30).getTime();
  assert.deepEqual(pickWindow({ kind: "recent" }, now), { from: now - 2 * 3600_000, to: now });
  const yday = addDays(startOfDay(now), -1);
  const w = pickWindow(wholeDay(yday), now);
  assert.equal(w.from, new Date(2026, 10, 15, 0, 0).getTime());
  assert.equal(w.to, new Date(2026, 10, 16, 0, 0).getTime() - 1);
  const today = pickWindow(wholeDay(now), now);
  assert.equal(today.to, now, "today stops at now");
  const morning = pickWindow(setHours(wholeDay(yday), 6, 9), now);
  assert.equal(morning.from, new Date(2026, 10, 15, 6).getTime());
  assert.equal(morning.to, new Date(2026, 10, 15, 9).getTime() - 1);
});

test("trail period: hours stay ordered and inside the day", () => {
  const d = wholeDay(new Date(2026, 10, 15).getTime());
  assert.deepEqual(setHours(d, 10, 8), { ...d, fromH: 10, toH: 11 });
  assert.deepEqual(setHours(d, 30, 40), { ...d, fromH: 23, toH: 24 });
});

test("trail period: a day sent from the animal screen becomes that day", () => {
  const day = new Date(2026, 10, 12).getTime();
  assert.deepEqual(pickFromWindow({ from: day, to: day + 24 * 3600_000 - 1 }), wholeDay(day));
});

test("trail period: date input round trip in local time", () => {
  const day = new Date(2026, 0, 5).getTime();
  assert.equal(isoDate(day), "2026-01-05");
  assert.equal(fromIsoDate("2026-01-05"), day);
  assert.equal(fromIsoDate(""), null);
});
