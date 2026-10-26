// ORUN offline map packs: pure catalog and bounds contract.
// This module does not download Esri/Google/OSMF public map tiles.
// Only prebuilt, redistributable, integrity-pinned PMTiles archives are valid.
export type OfflinePackKind = "protomaps-v4-vector" | "licensed-satellite-raster";
export type Bounds = [west: number, south: number, east: number, north: number];

export interface OfflinePack {
  id: string;
  version: string;
  title: string;
  kind: OfflinePackKind;
  bounds: Bounds;
  minZoom: number;
  maxZoom: number;
  bytes: number;
  sha256: string;
  url: string;
  attribution: string;
  license: string;
}

export interface InstalledPack {
  pack: OfflinePack;
  installedAt: number;
  fileName: string;
}

/** These are region *choices*, not downloaded packages or province borders. */
export const REQUESTED_REGIONS = [
  { id: "aydin", label: "Aydın" },
  { id: "mugla", label: "Muğla" },
] as const;

const MAX_BYTES = 2 * 1024 * 1024 * 1024; // first-stage single package limit
const idPattern = /^[a-z][a-z0-9-]{0,39}$/;
const versionPattern = /^[a-zA-Z0-9][a-zA-Z0-9._-]{0,39}$/;
const hashPattern = /^[0-9a-f]{64}$/;
const finite = (v: unknown): v is number => typeof v === "number" && Number.isFinite(v);
const record = (v: unknown): v is Record<string, unknown> =>
  !!v && typeof v === "object" && !Array.isArray(v);

export function parseOfflinePack(input: unknown): OfflinePack {
  if (!record(input)) throw new Error("Harita paketi nesne olmalı.");
  const id = input.id, version = input.version, title = input.title;
  const kind = input.kind, bytes = input.bytes, sha256 = input.sha256;
  const url = input.url, attribution = input.attribution, license = input.license;
  const b = input.bounds, minZoom = input.minZoom, maxZoom = input.maxZoom;

  if (typeof id !== "string" || !idPattern.test(id) ||
      typeof version !== "string" || !versionPattern.test(version))
    throw new Error("Harita paketi kimliği veya sürümü geçersiz.");
  if (typeof title !== "string" || !title.trim() || title.length > 90)
    throw new Error("Harita paketi adı geçersiz.");
  if (kind !== "protomaps-v4-vector" && kind !== "licensed-satellite-raster")
    throw new Error("Desteklenmeyen harita paketi türü.");
  if (!Number.isSafeInteger(bytes) || (bytes as number) < 127 || (bytes as number) > MAX_BYTES)
    throw new Error("Harita paketi boyutu güvenli sınırların dışında.");
  if (typeof sha256 !== "string" || !hashPattern.test(sha256))
    throw new Error("SHA-256 parmak izi gerekli.");
  if (typeof url !== "string" || url.length > 2048)
    throw new Error("Harita paketi adresi geçersiz.");
  let parsed: URL;
  try { parsed = new URL(url); }
  catch { throw new Error("Harita paketi adresi geçersiz."); }
  // No arbitrary file:/javascript:/http: package source in the phone. Bundles
  // will be hosted at an explicit HTTPS endpoint. Tailnet-only development
  // should use a TLS reverse proxy or import a prebuilt archive manually.
  if (parsed.protocol !== "https:" || !parsed.hostname || parsed.username || parsed.password ||
      parsed.hash || !parsed.pathname.endsWith(".pmtiles"))
    throw new Error("Harita paketleri yalnız HTTPS üzerinden alınır.");
  if (typeof attribution !== "string" || !attribution.trim() || attribution.length > 250 ||
      typeof license !== "string" || !license.trim() || license.length > 150)
    throw new Error("Harita kaynağının atıf ve lisans bilgileri gerekli.");

  if (!Array.isArray(b) || b.length !== 4 || !b.every(finite) ||
      b[0] < -180 || b[2] > 180 || b[1] < -85.051129 || b[3] > 85.051129 ||
      b[0] >= b[2] || b[1] >= b[3])
    throw new Error("Harita sınır koordinatları geçersiz.");
  if (!Number.isInteger(minZoom) || !Number.isInteger(maxZoom) ||
      (minZoom as number) < 0 || (maxZoom as number) > 20 ||
      (minZoom as number) > (maxZoom as number))
    throw new Error("Harita yakınlaştırma sınırları geçersiz.");
  if (kind === "protomaps-v4-vector" && (maxZoom as number) > 15)
    throw new Error("Protomaps v4 paket sınırı en fazla z15 olabilir.");

  return {
    id, version, title: title.trim(), kind,
    bounds: [...b] as Bounds, minZoom: minZoom as number, maxZoom: maxZoom as number,
    bytes: bytes as number, sha256, url: parsed.href, attribution: attribution.trim(),
    license: license.trim(),
  } as OfflinePack;
}

export function parseOfflineCatalog(input: unknown): OfflinePack[] {
  if (!record(input) || input.schema !== "orun-offline-packs-v1" || !Array.isArray(input.packs) ||
      input.packs.length > 100) throw new Error("Harita paket kataloğu geçersiz.");
  const seen = new Set<string>();
  return input.packs.map((raw: unknown) => {
    const pack = parseOfflinePack(raw);
    const key = pack.id + "@" + pack.version;
    if (seen.has(key)) throw new Error("Harita kataloğunda tekrarlanan paket var.");
    seen.add(key);
    return pack;
  });
}

export function packFileName(p: OfflinePack): string {
  // All components are strictly validated. The filename cannot traverse dirs.
  const v = parseOfflinePack(p);
  return `${v.id}-${v.version}-${v.sha256.slice(0, 16)}.pmtiles`;
}

export function contains(pack: OfflinePack, lon: number, lat: number): boolean {
  const [w, s, e, n] = pack.bounds;
  return finite(lon) && finite(lat) && lon >= w && lon <= e && lat >= s && lat <= n;
}

/** Prefer the smallest enclosing region with the highest actual source zoom.
 * Camera may overzoom to 20 without implying extra detail exists.
 */
export function chooseInstalledPack(packs: InstalledPack[], lon: number, lat: number): InstalledPack | null {
  const eligible = packs.filter((p) => contains(p.pack, lon, lat));
  eligible.sort((a, b) => b.pack.maxZoom - a.pack.maxZoom ||
    ((a.pack.bounds[2] - a.pack.bounds[0]) * (a.pack.bounds[3] - a.pack.bounds[1])) -
    ((b.pack.bounds[2] - b.pack.bounds[0]) * (b.pack.bounds[3] - b.pack.bounds[1])));
  return eligible[0] ?? null;
}

export interface StorageEstimate { quota?: number; usage?: number; }
/** Add safety margin for WebView metadata/cache and other application records. */
export function checkInstallSpace(pack: OfflinePack, estimate: StorageEstimate | null): string | null {
  if (!estimate || !finite(estimate.quota) || !finite(estimate.usage) ||
      estimate.usage < 0 || estimate.quota < estimate.usage)
    return "Telefondaki kullanılabilir depolama güvenilir biçimde ölçülemedi.";
  const remaining = estimate.quota - estimate.usage;
  const required = Math.ceil(pack.bytes * 1.1) + 32 * 1024 * 1024;
  return remaining >= required ? null : "Harita paketi için yeterli boş alan yok.";
}
