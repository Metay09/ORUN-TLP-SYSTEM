import assert from "node:assert/strict";
import { test } from "node:test";
import {
  areaHa, asGeoJSON, canonicalRing, importGeoJSON, loadZones, persistZones, ZONE_KEY, perimeterM, validateRing, validateSet,
  type Point, type Zone,
} from "../src/features/zones/geometry.ts";

const square: Point[] = [[28.054, 37.610], [28.056, 37.610], [28.056, 37.612], [28.054, 37.612]];
const zone = (ring: Point[], i = 1, active = true): Zone =>
  ({ id: String(i), name: `Mera ${i}`, active, ring });

test("zones: E7 geometry validates a pasture and preserves measured area", () => {
  assert.equal(validateRing(square), null);
  assert.ok(areaHa(square) > 3 && areaHa(square) < 5);
  assert.ok(perimeterM(square) > 700);
  const closed = [...square, square[0]];
  assert.equal(canonicalRing(closed).length, 4);
  assert.equal(validateRing(closed), null);
});
test("zones: rejects bowties, duplicate E7 points, zero-area and invalid geo domain", () => {
  const bowtie: Point[] = [[28,37], [28.002,37.002], [28,37.002], [28.002,37]];
  assert.notEqual(validateRing(bowtie), null);
  assert.notEqual(validateRing([[28,37],[28,37],[28.001,37.001]]), null);
  assert.notEqual(validateRing([[28,37],[28.001,37],[28.002,37]]), null);
  assert.notEqual(validateRing([[180,37],[179.99,37],[179.99,37.01]]), null);
  assert.notEqual(validateRing([[28,37],[39,37],[39,37.01]]), null);
  assert.notEqual(validateRing([[28,37],[28.000000001,37],[28,37.00001]]), null);
});
test("zones: 8 included polygons and 64 effective corners across all fields", () => {
  assert.equal(validateSet(Array.from({ length: 8 }, (_, i) => zone(square, i))), null);
  assert.match(validateSet(Array.from({ length: 9 }, (_, i) => zone(square, i)))!, /8 alan/);
  // Five 13-vertex pastures exceed the shared budget with 65 corners.
  const thirteen: Point[] = Array.from({ length: 13 }, (_, i) =>
    [28 + 0.001 * Math.cos(2 * Math.PI * i / 13),
     37 + 0.001 * Math.sin(2 * Math.PI * i / 13)] as Point);
  assert.match(validateSet(Array.from({ length: 5 }, (_, i) => zone(thirteen, i)))!, /64 köşe/);
  assert.equal(validateSet([...Array.from({ length: 8 }, (_, i) => zone(square, i)), zone(square, 99, false)]), null);
});
test("zones: GeoJSON round trip retains local-draft status, name and E7 coordinates", () => {
  const old = [zone(square), zone([[28,37],[28.001,37],[28,37.001]], 2, false)];
  const geo = asGeoJSON(old);
  assert.equal(geo.features[0].properties.orun_state, "LOCAL_DRAFT");
  const fresh = importGeoJSON(JSON.stringify(geo));
  assert.equal(fresh.length, 2);
  assert.deepEqual(fresh.map((x) => ({ name: x.name, active: x.active, ring: x.ring })),
                   old.map((x) => ({ name: x.name, active: x.active, ring: x.ring })));
  assert.notEqual(fresh[0].id, old[0].id);
});
test("zones: malformed and unsupported GeoJSON imports fail without partial commit", () => {
  assert.throws(() => importGeoJSON("{"), /JSON değil/);
  assert.throws(() => importGeoJSON(JSON.stringify({ type: "MultiPolygon", coordinates: [] })), /Polygon/);
  const valid = asGeoJSON([zone(square)]);
  const missingClose = structuredClone(valid);
  missingClose.features[0].geometry.coordinates[0].pop();
  assert.throws(() => importGeoJSON(JSON.stringify(missingClose)), /ilk ve son/);
  const tooMany = asGeoJSON(Array.from({ length: 9 }, (_, i) => zone(square, i)));
  assert.throws(() => importGeoJSON(JSON.stringify(tooMany)), /8 alan/);
});

test("zones: local draft survives reload, failed writes do not report a saved zone", () => {
  const own = Object.getOwnPropertyDescriptor(globalThis, "localStorage");
  const store = new Map<string, string>();
  Object.defineProperty(globalThis, "localStorage", {
    configurable: true, value: {
      getItem: (key: string) => store.get(key) ?? null,
      setItem: (key: string, value: string) => { store.set(key, value); },
    },
  });
  try {
    persistZones([zone(square)]);
    assert.ok(store.has(ZONE_KEY));
    assert.deepEqual(loadZones(), [zone(square)]);
    Object.defineProperty(globalThis, "localStorage", {
      configurable: true, value: { setItem: () => { throw new Error("storage full"); }, getItem: () => store.get(ZONE_KEY) },
    });
    assert.throws(() => persistZones([zone(square), zone(square, 2)]), /storage full/);
    assert.equal(loadZones().length, 1);
  } finally {
    if (own) Object.defineProperty(globalThis, "localStorage", own);
    else delete (globalThis as { localStorage?: Storage }).localStorage;
  }
});
