import assert from "node:assert/strict";
import { test } from "node:test";
import { pinDiameter, clusterDiameter, groupScreenPins, type ScreenPin } from "../src/features/map/pinLayout.ts";

test("map pins shrink on close zoom without vanishing, and stay small on far zoom", () => {
  assert.equal(pinDiameter(14), 30);
  assert.equal(pinDiameter(18), 22);
  assert.equal(pinDiameter(20), 19);
  assert.equal(pinDiameter(9), 18);
  assert.ok(pinDiameter(18, true) < 30);
  assert.equal(pinDiameter(18, true), pinDiameter(18) + 4);
  assert.ok(clusterDiameter(9) >= 24);
  assert.ok(clusterDiameter(20) <= 32);
});

test("map collision grouping leaves well-separated animal GPS pins distinct", () => {
  const xs: ScreenPin[] = [
    { id: "a", x: 20, y: 20 }, { id: "b", x: 100, y: 20 },
    { id: "c", x: 180, y: 20 },
  ];
  const groups = groupScreenPins(xs, 14);
  assert.equal(groups.length, 3);
  assert.deepEqual(groups.map((x) => x.ids), [["a"], ["b"], ["c"]]);
  assert.deepEqual(xs[0], { id: "a", x: 20, y: 20 });
});

test("map collision grouping shows one count even when pin positions overlap at max zoom", () => {
  const pins: ScreenPin[] = [
    { id: "a", x: 200, y: 100 }, { id: "b", x: 201, y: 100 },
    { id: "c", x: 330, y: 100 },
  ];
  const g = groupScreenPins(pins, 20);
  assert.equal(g.length, 2);
  assert.deepEqual(g[0].ids, ["a", "b"]);
  assert.deepEqual(g[1].ids, ["c"]);
});

test("map collision grouping preserves the selected animal inside a crowded group", () => {
  const pins: ScreenPin[] = [
    { id: "a", x: 10, y: 10 },
    { id: "selected", x: 20, y: 10, selected: true },
    { id: "b", x: 30, y: 10 },
    { id: "far", x: 300, y: 10 },
  ];
  const g = groupScreenPins(pins, 18);
  assert.equal(g.length, 2);
  assert.equal(g[0].selectedId, "selected");
  assert.deepEqual(g[0].ids, ["a", "selected", "b"]);
  assert.deepEqual(g[1].ids, ["far"]);
});

test("map collision grouping uses current projected zoom, not fixed metre distances", () => {
  const pins: ScreenPin[] = [{ id: "a", x: 0, y: 0 }, { id: "b", x: 35, y: 0 }];
  assert.equal(groupScreenPins(pins, 14).length, 1);
  assert.equal(groupScreenPins(pins, 20).length, 2);
  assert.deepEqual(groupScreenPins([], 20), []);
});
