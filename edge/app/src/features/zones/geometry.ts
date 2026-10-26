// ORUN local pasture drafts. Adapted from the user's ORUN-Saha.html
// OrunGeometry contract: WGS84 [longitude,latitude], E7 quantisation,
// BigInt segment predicates, 10-degree span and 8/64 device capacity.
// LOCAL DRAFTS ONLY: saving here is not a device confirmation.
export type Point = [number, number];
export type Ring = Point[];
export interface Zone { id: string; name: string; active: boolean; ring: Ring; }
export const ZONE_KEY = "orun.fences.v1"; // existing ORUN HTML prototype key
const E7 = 10_000_000;
const SPAN = 100_000_000;
const RAD = Math.PI / 180;
const R = 6_371_008.8;

function point(v: unknown): Point {
  if (!Array.isArray(v) || v.length !== 2 || !v.every((n) => typeof n === "number" && Number.isFinite(n)))
    throw new Error("Koordinat [boylam, enlem] şeklinde iki sayı olmalı.");
  const [x, y] = v as number[];
  if (Math.abs(x) > 180 || Math.abs(y) > 90)
    throw new Error("Enlem veya boylam geçersiz.");
  return [Math.round(x * E7), Math.round(y * E7)];
}
const equal = (a: Point, b: Point) => a[0] === b[0] && a[1] === b[1];

export function canonicalRing(raw: unknown): Ring {
  if (!Array.isArray(raw)) throw new Error("Köşeler liste olmalı.");
  const p = raw.map(point);
  while (p.length > 1 && equal(p[0], p[p.length - 1])) p.pop();
  return p.map(([x, y]) => [x / E7, y / E7]);
}

const cross = (a: Point, b: Point, c: Point) =>
  BigInt(b[0] - a[0]) * BigInt(c[1] - a[1]) -
  BigInt(b[1] - a[1]) * BigInt(c[0] - a[0]);
const onSegment = (a: Point, b: Point, p: Point) =>
  cross(a, b, p) === 0n &&
  p[0] >= Math.min(a[0], b[0]) && p[0] <= Math.max(a[0], b[0]) &&
  p[1] >= Math.min(a[1], b[1]) && p[1] <= Math.max(a[1], b[1]);
function intersect(a: Point, b: Point, c: Point, d: Point): boolean {
  const x = cross(a, b, c), y = cross(a, b, d), z = cross(c, d, a), w = cross(c, d, b);
  if (((x < 0n && y > 0n) || (x > 0n && y < 0n)) &&
      ((z < 0n && w > 0n) || (z > 0n && w < 0n))) return true;
  return (x === 0n && onSegment(a, b, c)) ||
         (y === 0n && onSegment(a, b, d)) ||
         (z === 0n && onSegment(c, d, a)) ||
         (w === 0n && onSegment(c, d, b));
}

export function validateRing(raw: unknown): string | null {
  let p: Point[];
  try { p = canonicalRing(raw).map(point); }
  catch (e) { return e instanceof Error ? e.message : String(e); }
  const n = p.length;
  if (n < 3) return "En az 3 köşe gerekli.";
  if (n > 64) return "Bir alanda en fazla 64 köşe olabilir.";
  for (let i = 0; i < n; i++) {
    const [x, y] = p[i], next = p[(i + 1) % n];
    if (Math.abs(x) >= 180 * E7 || Math.abs(y) >= 90 * E7)
      return "Kutup veya 180° meridyeni desteklenmiyor.";
    if (equal(p[i], next)) return "Komşu köşeler aynı noktada.";
    if (Math.abs(x - next[0]) > 180 * E7) return "180° meridyeni aşılamaz.";
  }
  for (let a = 0; a < 2; a++) {
    const vals = p.map((c) => c[a]);
    if (Math.max(...vals) - Math.min(...vals) > SPAN) return "Alan 10° sınırını aşıyor.";
  }
  // Translate around the first vertex to mirror firmware bounded area math.
  const [ox, oy] = p[0]; let signed = 0n;
  for (let i = 0; i < n; i++) {
    const a = p[i], b = p[(i + 1) % n];
    signed += BigInt(a[0] - ox) * BigInt(b[1] - oy) -
              BigInt(a[1] - oy) * BigInt(b[0] - ox);
  }
  if (signed === 0n) return "Alan sıfır; köşeler bir alan oluşturmuyor.";
  for (let i = 0; i < n; i++) for (let j = i + 1; j < n; j++) {
    if (j === i + 1 || (i === 0 && j === n - 1)) continue;
    if (intersect(p[i], p[(i + 1) % n], p[j], p[(j + 1) % n]))
      return "Alan kenarları kesişiyor veya üst üste geliyor.";
  }
  return null;
}

export function validateSet(zones: Zone[]): string | null {
  let selected = 0, vertices = 0;
  for (const z of zones) {
    const e = validateRing(z.ring);
    if (e) return `${z.name}: ${e}`;
    if (z.active) { selected++; vertices += canonicalRing(z.ring).length; }
  }
  if (selected > 8) return "En fazla 8 alan taslağa dahil edilebilir.";
  if (vertices > 64) return `Toplam en fazla 64 köşe olabilir (şu an ${vertices}).`;
  return null;
}

/**
 * Safely reduce a valid polygon to a device's remaining vertex budget.
 * Never mutates input; rejects candidates that change the measured area by
 * more than `maxLossPct` or break firmware-compatible topology.
 * Unlike geometric libraries that blindly simplify, every step is validated.
 */
export function simplifyRingToBudget(
  input: Ring, maxVertices: number, maxLossPct = 2,
): { ring: Ring; areaDeltaPct: number } | null {
  if (!Number.isInteger(maxVertices) || maxVertices < 3 || maxVertices > 64 ||
      !Number.isFinite(maxLossPct) || maxLossPct < 0) return null;
  const original = canonicalRing(input);
  if (validateRing(original)) return null;
  const initialArea = areaHa(original);
  if (!(initialArea > 0)) return null;
  let current = original;
  while (current.length > maxVertices) {
    let best: Ring | null = null;
    let bestCost = Infinity;
    for (let i = 0; i < current.length; i++) {
      const prev = current[(i - 1 + current.length) % current.length];
      const mid = current[i];
      const next = current[(i + 1) % current.length];
      const candidate = current.filter((_, j) => j !== i);
      if (validateRing(candidate)) continue;
      const deltaPct = Math.abs(areaHa(candidate) - initialArea) / initialArea * 100;
      if (deltaPct > maxLossPct) continue;
      const excess = lengthM(prev, mid) + lengthM(mid, next) - lengthM(prev, next);
      const cost = deltaPct * 1000 + excess;
      if (cost < bestCost) { bestCost = cost; best = candidate; }
    }
    if (!best) return null;
    current = best;
  }
  return { ring: current, areaDeltaPct: Math.abs(areaHa(current) - initialArea) / initialArea * 100 };
}

/** Preview follows the *actual map centre* before committing a new vertex.
 * Never mutates the saved/draft ring. A preview is never a device instruction. */
export function previewAtCentre(draft: Ring, centre: Point | null): Ring {
  return centre && draft.length ? [...draft, centre] : [...draft];
}

export function areaHa(ring: Ring): number {
  if (ring.length < 3) return 0;
  const p = canonicalRing(ring);
  const ref = Math.sin(p[0][1] * RAD);
  let sum = 0;
  for (let i = 0; i < p.length; i++) {
    const a = p[i], b = p[(i + 1) % p.length];
    sum += (b[0] - a[0]) * RAD *
      (Math.sin(a[1] * RAD) + Math.sin(b[1] * RAD) - 2 * ref);
  }
  return Math.abs(sum) * R * R / 20_000;
}
function lengthM(a: Point, b: Point): number {
  const lat = (b[1] - a[1]) * RAD / 2, lon = (b[0] - a[0]) * RAD / 2;
  const h = Math.sin(lat) ** 2 + Math.cos(a[1] * RAD) * Math.cos(b[1] * RAD) * Math.sin(lon) ** 2;
  return 2 * R * Math.asin(Math.min(1, Math.sqrt(Math.max(0, h))));
}
export function perimeterM(ring: Ring): number {
  if (ring.length < 2) return 0;
  return ring.reduce((s, p, i) => s + (i === 0 && ring.length < 3 ? 0 : lengthM(p, ring[(i + 1) % ring.length])), 0);
}
export function asGeoJSON(zones: Zone[]) {
  return {
    type: "FeatureCollection",
    features: zones.map((z) => ({
      type: "Feature", properties: { name: z.name, active: z.active, orun_state: "LOCAL_DRAFT" },
      geometry: { type: "Polygon", coordinates: [[...z.ring, z.ring[0]]] },
    })),
  };
}
export function importGeoJSON(content: string): Zone[] {
  let data: unknown;
  try { data = JSON.parse(content); } catch { throw new Error("GeoJSON geçerli JSON değil."); }
  const d = data as Record<string, unknown> | null;
  if (!d || typeof d !== "object" || Array.isArray(d) || d.crs) throw new Error("WGS84 GeoJSON gerekli.");
  const arr = d.type === "FeatureCollection" ? d.features :
    d.type === "Feature" ? [d] : [{ type: "Feature", geometry: d, properties: {} }];
  if (!Array.isArray(arr) || !arr.length || arr.length > 64) throw new Error("GeoJSON içinde 1–64 alan gerekli.");
  const zones = arr.map((entry, i) => {
    const f = entry as Record<string, unknown>;
    const g = f?.geometry as Record<string, unknown> | undefined;
    if (f?.type !== "Feature" || f.crs || g?.type !== "Polygon" ||
        !Array.isArray(g.coordinates) || g.coordinates.length !== 1)
      throw new Error(`${i + 1}. alan: deliksiz Polygon gerekli.`);
    const raw = g.coordinates[0] as unknown;
    if (!Array.isArray(raw) || raw.length < 4 || !equal(point(raw[0]), point(raw[raw.length - 1])))
      throw new Error(`${i + 1}. alan: ilk ve son köşe aynı olmalı.`);
    const ring = canonicalRing(raw);
    const prop = (f.properties ?? {}) as Record<string, unknown>;
    if (!prop || typeof prop !== "object" || Array.isArray(prop) ||
        (prop.active !== undefined && typeof prop.active !== "boolean"))
      throw new Error(`${i + 1}. alan: özellikler geçersiz.`);
    const name = typeof prop.name === "string" && prop.name.trim() ? prop.name.trim().slice(0, 40) : `Alan ${i + 1}`;
    return { id: crypto.randomUUID(), name, active: prop.active !== false, ring };
  });
  const error = validateSet(zones);
  if (error) throw new Error(error);
  return zones;
}

/** Phone-only, legacy-compatible draft decoder. Refuse to overwrite data
 * that might be partially corrupt: its raw JSON must remain recoverable. */
function decodeStoredZones(raw: unknown): Zone[] {
  if (!Array.isArray(raw) || raw.length > 100) throw new Error("Alan listesi geçersiz.");
  const zones = raw.map((value: unknown): Zone => {
    const z = value as Record<string, unknown> | null;
    if (!z || typeof z !== "object" || Array.isArray(z) ||
        typeof z.id !== "string" || !z.id ||
        typeof z.name !== "string" || !z.name.trim() ||
        (z.active !== undefined && typeof z.active !== "boolean"))
      throw new Error("Eski alan kaydının bilgileri geçersiz.");
    return { id: z.id, name: z.name, active: z.active !== false, ring: canonicalRing(z.ring) };
  });
  const error = validateSet(zones);
  if (error) throw new Error(error);
  return zones;
}

export function zoneStorageWarning(): string | null {
  try {
    const raw = localStorage.getItem(ZONE_KEY);
    if (raw === null) return null;
    decodeStoredZones(JSON.parse(raw));
    return null;
  } catch {
    return "Önceki mera kayıtları okunamadı. Güvenlik için üzerine yazılmayacak; eski veriyi silmeyin.";
  }
}

/** Browser/Capacitor WebView persistence, independent of demo livestock. */
export function loadZones(): Zone[] {
  try {
    const raw = localStorage.getItem(ZONE_KEY);
    return raw === null ? [] : decodeStoredZones(JSON.parse(raw));
  } catch { return []; }
}
export function persistZones(zones: Zone[]): void {
  const error = validateSet(zones);
  if (error) throw new Error(error);
  // A prior app/prototype might have written records not supported by this
  // version. Never silently replace them with an empty or partial list.
  const raw = localStorage.getItem(ZONE_KEY);
  if (raw !== null) {
    try { decodeStoredZones(JSON.parse(raw)); }
    catch { throw new Error("Eski mera kayıtları okunamadı; üzerlerine yazılmadı. Verileri silmeyin."); }
  }
  localStorage.setItem(ZONE_KEY, JSON.stringify(zones));
}
