// ORUN satellite imagery resilience. Esri high-zoom tiles can be missing in
// some regions. Keep an independent lower-zoom raster under every detail tile
// so a missing/204/404 detail tile does not leave blank pasture on the map.
// Source maxzoom (raster tile request cap) is not the map camera maxZoom.
import type { StyleSpecification } from "maplibre-gl";

export const SATELLITE_TILE_URL =
  "https://server.arcgisonline.com/ArcGIS/rest/services/World_Imagery/MapServer/tile/{z}/{y}/{x}";
export const SATELLITE_ATTRIBUTION = "Görüntü © Esri, Maxar, Earthstar Geographics";
export const SATELLITE_FALLBACK_ZOOM = 15;
export const SATELLITE_DETAIL_ZOOM = 19;

export function satelliteRasterParts(primaryId = "base"): {
  sources: StyleSpecification["sources"];
  layers: StyleSpecification["layers"];
} {
  const fallbackId = `${primaryId}-fallback`;
  const tiles = [SATELLITE_TILE_URL];

  // The fallback *remains a visible layer* even when fine-grained tiles are
  // unavailable. MapLibre automatically enlarges tiles above source maxzoom.
  // Keep attribution on the always-visible fallback source to avoid repeats.
  return {
    sources: {
      [fallbackId]: {
        type: "raster", tiles, tileSize: 256,
        maxzoom: SATELLITE_FALLBACK_ZOOM, attribution: SATELLITE_ATTRIBUTION,
      },
      [primaryId]: {
        type: "raster", tiles, tileSize: 256, maxzoom: SATELLITE_DETAIL_ZOOM,
      },
    },
    layers: [
      { id: fallbackId, type: "raster", source: fallbackId },
      { id: primaryId, type: "raster", source: primaryId,
        minzoom: SATELLITE_FALLBACK_ZOOM,
        paint: { "raster-fade-duration": 150 } },
    ],
  };
}
