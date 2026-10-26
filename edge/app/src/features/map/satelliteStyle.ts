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

export function satelliteRasterParts(primaryId = "base", safeMode = false): {
  sources: StyleSpecification["sources"];
  layers: StyleSpecification["layers"];
} {
  const fallbackId = `${primaryId}-fallback`;
  const tiles = [SATELLITE_TILE_URL];

  // ArcGIS can return an *opaque grey JPEG* ("Map data not yet available")
  // with HTTP 200 when high-detail tiles are absent. The z19 layer then
  // covers the valid z15 underlay. Safe mode removes that layer entirely
  // and lets MapLibre overzoom the z15 imagery up to camera zoom 20.
  // Source maxzoom is never a camera zoom limit; this is only a fallback
  // for display, not a claim that new satellite detail exists.
  if (safeMode) {
    return {
      sources: {
        [primaryId]: {
          type: "raster", tiles, tileSize: 256,
          maxzoom: SATELLITE_FALLBACK_ZOOM, attribution: SATELLITE_ATTRIBUTION,
        },
      },
      layers: [{ id: primaryId, type: "raster", source: primaryId }],
    };
  }

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
