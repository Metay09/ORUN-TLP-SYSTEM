import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import test from "node:test";

// Structural regressions keep this optimization limited to label visibility;
// rendering and actual frame times still need Android device validation.
const code = readFileSync(new URL("../src/features/map/mapEngine.ts", import.meta.url), "utf8");

test("satellite labels stay installed but can start hidden", () => {
  const raster = code.slice(code.indexOf("function rasterStyle("), code.indexOf("const FALLBACK:"));
  assert.match(raster, /sources\.labels = \{ type: "raster"/);
  assert.match(raster, /id: "labels", type: "raster", source: "labels"/);
  assert.match(raster, /visibility: labels \? "visible" : "none"/);
  assert.doesNotMatch(raster, /if \(labels\) \{/);
});

test("toggling labels changes layer visibility, not style identity", () => {
  const setBase = code.slice(code.indexOf("async setBase("), code.indexOf("setTilt("));
  assert.match(setBase, /const key = offline/);
  const styleIdentity = setBase.slice(setBase.indexOf("const key = offline"), setBase.indexOf("this.base = base;"));
  assert.doesNotMatch(styleIdentity, /layers\.labels/);
  assert.match(code, /vis\("labels", l\.labels && this\.base === "satellite" && !this\.drawnOffline\)/);
  const setLayers = code.slice(code.indexOf("setLayers("), code.indexOf("// ---------------------------------------------------------------- data --"));
  assert.doesNotMatch(setLayers, /relabel|setBase\(/);
});

test("real base-style changes still use established fallback and style replacement", () => {
  assert.match(code, /this\.map\.setStyle\(rasterStyle\(base, layers\.labels, s\), \{ diff: false \}\)/);
  assert.match(code, /this\.map\.setStyle\(this\.streetCache, \{ diff: false \}\)/);
  assert.match(code, /this\.map\.setStyle\(FALLBACK, \{ diff: false \}\)/);
});
