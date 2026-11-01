// 2: opens the installed pack for MapLibre and tells the map when packs change.
import { addProtocol } from "maplibre-gl";
import { FileSource, PMTiles, Protocol } from "pmtiles";
import { chooseInstalledPack } from "./catalog.ts";
import type { OfflineMap } from "./offlineStyle.ts";
import { installedPackFile, listInstalledPacks, supportsOfflineMapStorage } from "./store.ts";

let protocol: Protocol | null = null;
const listeners = new Set<() => void>();

export function onOfflineMapsChanged(fn: () => void): () => void {
  listeners.add(fn);
  return () => { listeners.delete(fn); };
}

export function offlineMapsChanged(): void {
  for (const fn of listeners) fn();
}

/** The installed pack covering (lon, lat), or any installed pack; null if none. */
export async function openOfflineMap(lon: number, lat: number): Promise<OfflineMap | null> {
  if (!supportsOfflineMapStorage()) return null;
  const packs = await listInstalledPacks();
  const pick = chooseInstalledPack(packs, lon, lat) ?? packs[0] ?? null;
  if (!pick) return null;
  const file = await installedPackFile(pick);
  if (!protocol) {
    protocol = new Protocol();
    addProtocol("pmtiles", protocol.tile);
  }
  const archive = new PMTiles(new FileSource(file));
  protocol.add(archive);
  return {
    url: "pmtiles://" + archive.source.getKey(),
    attribution: pick.pack.attribution,
    bounds: pick.pack.bounds,
    maxZoom: pick.pack.maxZoom,
    title: pick.pack.title,
  };
}
