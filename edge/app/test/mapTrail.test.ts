// Map display geometry: no invented jumps or radio-gap paths.
import assert from "node:assert/strict";
import { test } from "node:test";
import type { Observation } from "../src/app/model.ts";
import { readableTrail } from "../src/features/map/routeGeometry.ts";

const point = (seconds: number, lat: number, lon: number, seq = seconds): Observation => ({
  tracker: "A", seq, time: new Date(Date.UTC(2026, 10, 10) + seconds * 1000).toISOString(),
  lat, lon, period_s: 60,
} as Observation);

test("map trail: long silence is a break, not a straight dashed route", () => {
  const p = [point(0, 37, 28), point(60, 37.0001, 28), point(7200, 37.0002, 28)];
  const s = readableTrail(p);
  assert.equal(s.breaks, 1);
  assert.deepEqual(s.runs.map((r) => r.length), [2, 1]);
});

test("map trail: GPS jump is split while the original points remain unchanged", () => {
  const p = [point(0, 37, 28), point(60, 37.0001, 28), point(120, 37.02, 28)];
  const s = readableTrail(p);
  assert.equal(s.breaks, 1);
  assert.equal(s.runs.length, 2);
  assert.equal(p.length, 3);
});

test("map trail: straight route drops intermediate visual markers, preserves endpoints", () => {
  const p = Array.from({ length: 20 }, (_, i) => point(i * 60, 37, 28 + i * 0.0001));
  const s = readableTrail(p);
  assert.equal(s.breaks, 0);
  assert.equal(s.runs[0].length, 2);
  assert.equal(s.runs[0][0], p[0]);
  assert.equal(s.runs[0][1], p[p.length - 1]);
});

test("map trail: repeated timestamp is not an extra segment", () => {
  const p = [point(0, 37, 28), point(0, 37.001, 28, 200), point(60, 37.0001, 28)];
  const s = readableTrail(p);
  assert.equal(s.breaks, 0);
  assert.equal(s.runs[0].length, 2);
});

test("map trail: a real corner remains visible instead of being straightened", () => {
  const p = [point(0, 37, 28), point(60, 37.0003, 28), point(120, 37.0003, 28.0003)];
  const s = readableTrail(p);
  assert.equal(s.breaks, 0);
  assert.equal(s.runs[0].length, 3);
});
