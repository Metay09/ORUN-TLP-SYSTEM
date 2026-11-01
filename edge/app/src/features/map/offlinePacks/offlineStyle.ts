// 2: the map drawn from the phone's own Aydın–Muğla pack, with no network.
// Fonts and icons ship inside the app (public/map-assets); characters outside
// the shipped ranges are drawn locally by MapLibre.
import { layers, namedFlavor } from "@protomaps/basemaps";
import type { StyleSpecification } from "maplibre-gl";
import type { Bounds } from "./catalog.ts";

export const OFFLINE_SOURCE = "orun-offline";

/** An installed pack opened for drawing. */
export interface OfflineMap {
  url: string;            // pmtiles://<key>
  attribution: string;
  bounds: Bounds;
  maxZoom: number;
  title: string;
}

export function offlineStyle(map: OfflineMap, origin: string): StyleSpecification {
  return {
    version: 8,
    glyphs: `${origin}/map-assets/fonts/{fontstack}/{range}.pbf`,
    sprite: `${origin}/map-assets/sprites/light`,
    sources: {
      [OFFLINE_SOURCE]: { type: "vector", url: map.url, attribution: map.attribution },
    },
    layers: layers(OFFLINE_SOURCE, namedFlavor("light"), { lang: "tr" }),
  };
}
