import assert from "node:assert/strict";
import test from "node:test";
import { elevationMode } from "../src/features/map/elevation.ts";

test("3B and relief both off: no DEM needed on first load", () => {
  assert.deepEqual(elevationMode(false, false, false, "satellite", false),
    { terrain: false, shade: false, needsDEM: false });
});

test("relief alone uses the DEM without enabling 3B", () => {
  assert.deepEqual(elevationMode(false, false, true, "terrain", false),
    { terrain: false, shade: true, needsDEM: true });
});

test("3B alone uses the DEM without enabling relief", () => {
  assert.deepEqual(elevationMode(true, false, false, "satellite", false),
    { terrain: true, shade: false, needsDEM: true });
});

test("3B and relief together share the DEM", () => {
  assert.deepEqual(elevationMode(true, false, true, "terrain", false),
    { terrain: true, shade: true, needsDEM: true });
});

test("street suppresses relief without disabling 3B", () => {
  assert.deepEqual(elevationMode(true, false, true, "street", false),
    { terrain: true, shade: false, needsDEM: true });
});

test("offline vector suppresses relief without disabling 3B", () => {
  assert.deepEqual(elevationMode(true, false, true, "satellite", true),
    { terrain: true, shade: false, needsDEM: true });
});

test("drawing temporarily flattens 3B but does not change relief preference", () => {
  assert.deepEqual(elevationMode(true, true, true, "satellite", false),
    { terrain: false, shade: true, needsDEM: true });
});

test("drawing with relief disabled needs no DEM; 3B may resume later", () => {
  assert.deepEqual(elevationMode(true, true, false, "satellite", false),
    { terrain: false, shade: false, needsDEM: false });
});
