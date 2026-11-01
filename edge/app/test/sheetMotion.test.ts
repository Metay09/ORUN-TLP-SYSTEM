import assert from "node:assert/strict";
import { test } from "node:test";
import { clampSheetHeight, settleSheet, snapHeight } from "../src/features/map/sheetMotion.ts";

const H = 800, PEEK = 150, HALF = 0.46;
const compact = snapHeight("peek", H, PEEK, HALF);
const middle = snapHeight("half", H, PEEK, HALF);
const full = snapHeight("full", H, PEEK, HALF);

test("bottom sheet retains same three heights, and drag cannot vanish below compact", () => {
  assert.deepEqual([compact, middle, full], [150, 368, 728]);
  assert.equal(clampSheetHeight(-200, H, PEEK, HALF), compact);
  assert.equal(clampSheetHeight(2000, H, PEEK, HALF), full);
  assert.equal(clampSheetHeight(270, H, PEEK, HALF), 270);
});

test("bottom sheet: small taps and accidental finger shifts do not change the panel", () => {
  assert.equal(settleSheet("peek", 0, compact, H, PEEK, HALF), "peek");
  assert.equal(settleSheet("peek", 11, compact + 11, H, PEEK, HALF), "peek");
  assert.equal(settleSheet("half", -10, middle - 10, H, PEEK, HALF), "half");
  assert.equal(settleSheet("full", 0, full, H, PEEK, HALF), "full");
});

test("bottom sheet: short deliberate upward drag easily opens compact panel", () => {
  // Nearest-distance snapping used to drop a 35px swipe back into compact.
  assert.equal(settleSheet("peek", 35, compact + 35, H, PEEK, HALF), "half");
  assert.equal(settleSheet("half", 35, middle + 35, H, PEEK, HALF), "full");
});

test("bottom sheet: downward swipe closes one stage, never unexpectedly to compact", () => {
  assert.equal(settleSheet("full", -35, full - 35, H, PEEK, HALF), "half");
  assert.equal(settleSheet("half", -35, middle - 35, H, PEEK, HALF), "peek");
  assert.equal(settleSheet("peek", -35, compact, H, PEEK, HALF), "peek");
});

test("bottom sheet: long deliberate swipes may traverse two stages", () => {
  assert.equal(settleSheet("peek", 590, full - 10, H, PEEK, HALF), "full");
  assert.equal(settleSheet("full", -550, compact + 28, H, PEEK, HALF), "peek");
});

test("bottom sheet: quick 12px flick works, slow 12px move stays put", () => {
  assert.equal(settleSheet("peek", 12, compact + 12, H, PEEK, HALF, 0.6), "half");
  assert.equal(settleSheet("full", -12, full - 12, H, PEEK, HALF, -0.6), "half");
  assert.equal(settleSheet("peek", 12, compact + 12, H, PEEK, HALF, 0.1), "peek");
});

test("bottom sheet: no nonexistent snap when stops overlap in short landscape map", () => {
  const total = 200, peek = 150;
  assert.deepEqual(["peek", "half", "full"].map((s) => snapHeight(s as "peek"|"half"|"full", total, peek)), [128, 128, 128]);
  assert.equal(settleSheet("peek", 100, 128, total, peek), "peek");
  assert.equal(settleSheet("full", -100, 128, total, peek), "full");
});

test("bottom sheet: gesture calculations are robust to broken pointer values", () => {
  assert.equal(settleSheet("peek", NaN, NaN, H, PEEK, HALF), "peek");
  assert.equal(settleSheet("half", Number.POSITIVE_INFINITY, middle, H, PEEK, HALF), "half");
});
