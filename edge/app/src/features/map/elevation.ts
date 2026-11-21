// Shared DEM policy for MapLibre: 3B and hillshade remain independent.
// A single raster-dem source is used when one or both need elevation data.
// Do not change base map, persisted preferences, or any animal/Gateway data.
import type { BaseMap } from "../../app/stores.ts";

export interface ElevationMode {
  terrain: boolean;
  shade: boolean;
  needsDEM: boolean;
}

export function elevationMode(tilt: boolean, drawing: boolean, relief: boolean,
  base: BaseMap, offline: boolean): ElevationMode {
  // Field boundaries are edited flat; restore 3B after drawing ends.
  const terrain = tilt && !drawing;
  // Preserve existing rule: no hillshade on street or the offline vector map.
  const shade = relief && base !== "street" && !offline;
  return { terrain, shade, needsDEM: terrain || shade };
}
