import assert from "node:assert/strict";
import { test } from "node:test";
import { pinDiameter, clusterDiameter, groupScreenPins, type ScreenPin } from "../src/features/map/pinLayout.ts";

test("map pins grow gradually with zoom; collision diameter equals actual CSS scale", () => {
  assert.equal(pinDiameter(9), 18);
  assert.equal(pinDiameter(14), 26);
  assert.equal(pinDiameter(18), 33);
  assert.equal(pinDiameter(20), 36);
  assert.ok(pinDiameter(14) < pinDiameter(18));
  assert.equal(pinDiameter(18, true), 58);
  assert.ok(clusterDiameter(9) >= 24);
  assert.ok(clusterDiameter(20) <= 40);
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
  // The same GPS separation occupies more screen pixels when zooming in.
  assert.equal(groupScreenPins([{ id: "a", x: 0, y: 0 }, { id: "b", x: 35, y: 0 }], 14).length, 1);
  assert.equal(groupScreenPins([{ id: "a", x: 0, y: 0 }, { id: "b", x: 70, y: 0 }], 20).length, 2);
  assert.deepEqual(groupScreenPins([], 20), []);
});

test("map collision groups stay local instead of swallowing a long row of animals", () => {
  const pins: ScreenPin[] = [0, 35, 70, 105, 140].map((x, i) => ({
    id: String(i), x, y: 50,
  }));
  const groups = groupScreenPins(pins, 14);
  assert.deepEqual(groups.map((g) => g.ids), [["0", "1"], ["2", "3"], ["4"]]);
  assert.equal(groups.reduce((sum, group) => sum + group.ids.length, 0), pins.length);
});

test("map collision grouping handles selected 58px marker without hiding it", () => {
  const pins: ScreenPin[] = [
    { id: "selected", x: 0, y: 0, selected: true },
    { id: "near", x: 40, y: 0 },
    { id: "far", x: 120, y: 0 },
  ];
  const groups = groupScreenPins(pins, 20);
  assert.deepEqual(groups.map((g) => g.ids), [["selected", "near"], ["far"]]);
  assert.equal(groups[0].selectedId, "selected");
  assert.deepEqual(pins[0], { id: "selected", x: 0, y: 0, selected: true });
});
