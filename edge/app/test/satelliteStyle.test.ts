import assert from "node:assert/strict";
import { test } from "node:test";
import {
  satelliteRasterParts, SATELLITE_ATTRIBUTION, SATELLITE_DETAIL_ZOOM,
  SATELLITE_FALLBACK_ZOOM, SATELLITE_TILE_URL,
} from "../src/features/map/satelliteStyle.ts";

test("satellite: lower zoom imagery stays underneath missing high-detail map tiles", () => {
  const parts = satelliteRasterParts("base");
  assert.equal(parts.layers.length, 2);
  assert.equal(parts.layers[0].id, "base-fallback");
  assert.equal(parts.layers[1].id, "base");
  assert.equal(parts.layers[1].minzoom, SATELLITE_FALLBACK_ZOOM);

  const low = parts.sources["base-fallback"];
  const high = parts.sources.base;
  assert.equal(low.type, "raster");
  assert.equal(high.type, "raster");
  if (low.type !== "raster" || high.type !== "raster") return;
  assert.equal(low.maxzoom, 15);
  assert.equal(high.maxzoom, 19);
  assert.deepEqual(low.tiles, [SATELLITE_TILE_URL]);
  assert.deepEqual(high.tiles, low.tiles);
  assert.equal(low.attribution, SATELLITE_ATTRIBUTION);
  assert.equal(SATELLITE_DETAIL_ZOOM, 19);
});

test("satellite: pasture crosshair can use the same no-blank underlay", () => {
  const parts = satelliteRasterParts("satellite");
  assert.equal(parts.layers[0].id, "satellite-fallback");
  assert.equal(parts.layers[1].id, "satellite");
  assert.ok(parts.sources["satellite-fallback"]);
  assert.ok(parts.sources.satellite);
  assert.equal(parts.layers[0].type, "raster");
  assert.equal(parts.layers[1].type, "raster");
  // No upper bound on visible layers; MapLibre overzooms 15 when detail
  // tiles at 17/18/19 are missing, and likewise at map zoom 20.
  assert.equal(parts.layers[0].maxzoom, undefined);
  assert.equal(parts.layers[1].maxzoom, undefined);
});

test("satellite: safe mode cannot paint opaque error-text detail tiles over the valid z15 source", () => {
  for (const id of ["base", "satellite"]) {
    const parts = satelliteRasterParts(id, true);
    assert.equal(parts.layers.length, 1);
    assert.equal(parts.layers[0].id, id);
    const source = parts.sources[id];
    assert.equal(source.type, "raster");
    if (source.type !== "raster") continue;
    assert.equal(source.maxzoom, SATELLITE_FALLBACK_ZOOM);
    assert.deepEqual(source.tiles, [SATELLITE_TILE_URL]);
    assert.equal(source.attribution, SATELLITE_ATTRIBUTION);
  }
});
