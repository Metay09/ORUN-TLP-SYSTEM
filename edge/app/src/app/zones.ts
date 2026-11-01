// Alanlar (pasture boundaries): pure geometry and rules, no DOM.
// Tested in test/zones.test.ts.
//
// Validation and point classification mirror the collar firmware
// (firmware/src/geofence_geometry.cpp) bit for bit: the same E7 integer
// rounding, the same rejection reasons and the same winding-number test, so a
// boundary the phone accepts is one the collar accepts, and "inside" means the
// same thing on both. Limits come from firmware geofence_format.h /
// geofence_geometry.h.
//
// Historical outsideEpisodes is a pure diagnostic helper only. Real tracker
// geofence OUTSIDE alerts must originate from authenticated device EVENTs;
// phone records never grant alarm authority.
import { distanceM, type LatLon } from "./geo.ts";

export const FENCE = { maxAreas: 8, maxTotalVertices: 64, maxPolygonVertices: 64, maxSpanE7: 100_000_000 } as const;

/** [lon, lat] like GeoJSON. */
export type LonLat = [number, number];

export interface Zone {
  id: string;
  name: string;
  ring: LonLat[];     // open ring (no closing duplicate)
  active: boolean;    // part of the set the herd is checked against
  updated: number;    // epoch ms of the last change
}

/** Serbest otlatma: zone alerts silenced until `until` (ms), or until turned off. */
export interface Graze { until: number | null; forever: boolean; }
export const NO_GRAZE: Graze = { until: null, forever: false };

export function grazing(g: Graze, now: number): boolean {
  return g.forever || (g.until !== null && g.until > now);
}

export interface ZoneBook { zones: Zone[]; graze: Graze; }

// ------------------------------------------------------------------ E7 ---
export type E7 = [number, number];   // [latitude_e7, longitude_e7]

export const toE7 = ([lon, lat]: LonLat): E7 => [Math.round(lat * 1e7), Math.round(lon * 1e7)];

/** E7 vertices as the collar would store them (closing duplicate dropped). */
export function canonicalE7(ring: LonLat[]): E7[] {
  const v = ring.map(toE7);
  if (v.length > 1 && v[0][0] === v[v.length - 1][0] && v[0][1] === v[v.length - 1][1]) v.pop();
  return v;
}

export type ZoneError = "tooFew" | "tooMany" | "range" | "span" | "dup" | "degenerate" | "selfX";

// x is longitude, y is latitude (firmware cross()). BigInt: deltas reach 1e8,
// products 1e16, beyond exact doubles.
function cross(a: E7, b: E7, p: E7): bigint {
  const abx = BigInt(b[1] - a[1]), aby = BigInt(b[0] - a[0]);
  const apx = BigInt(p[1] - a[1]), apy = BigInt(p[0] - a[0]);
  return abx * apy - aby * apx;
}
function orient(a: E7, b: E7, p: E7): number {
  const c = cross(a, b, p);
  return c < 0n ? -1 : c > 0n ? 1 : 0;
}
const between = (v: number, a: number, b: number) => v >= Math.min(a, b) && v <= Math.max(a, b);
function onSegment(a: E7, b: E7, p: E7): boolean {
  return orient(a, b, p) === 0 && between(p[0], a[0], b[0]) && between(p[1], a[1], b[1]);
}
function segmentsIntersect(a: E7, b: E7, c: E7, d: E7): boolean {
  const o1 = orient(a, b, c), o2 = orient(a, b, d), o3 = orient(c, d, a), o4 = orient(c, d, b);
  if (o1 !== o2 && o3 !== o4) return true;
  return (o1 === 0 && onSegment(a, b, c)) || (o2 === 0 && onSegment(a, b, d)) ||
    (o3 === 0 && onSegment(c, d, a)) || (o4 === 0 && onSegment(c, d, b));
}
const adjacent = (i: number, j: number, n: number) => i === j || (i + 1) % n === j || (j + 1) % n === i;
const inRange = (p: E7) => p[0] > -900_000_000 && p[0] < 900_000_000 && p[1] > -1_800_000_000 && p[1] < 1_800_000_000;

/** null when the collar accepts the polygon; otherwise the firmware's reason. */
export function validateE7(v: E7[]): ZoneError | null {
  const n = v.length;
  if (n < 3) return "tooFew";
  if (n > FENCE.maxPolygonVertices) return "tooMany";
  let la0 = v[0][0], la1 = v[0][0], lo0 = v[0][1], lo1 = v[0][1];
  for (let i = 0; i < n; i++) {
    const c = v[i], d = v[(i + 1) % n];
    if (!inRange(c)) return "range";
    if (c[0] === d[0] && c[1] === d[1]) return "dup";
    la0 = Math.min(la0, c[0]); la1 = Math.max(la1, c[0]); lo0 = Math.min(lo0, c[1]); lo1 = Math.max(lo1, c[1]);
  }
  if (la1 - la0 > FENCE.maxSpanE7 || lo1 - lo0 > FENCE.maxSpanE7) return "span";
  const ox = BigInt(v[0][1]), oy = BigInt(v[0][0]);
  let s = 0n;
  for (let i = 0; i < n; i++) {
    const a = v[i], b = v[(i + 1) % n];
    s += (BigInt(a[1]) - ox) * (BigInt(b[0]) - oy) - (BigInt(a[0]) - oy) * (BigInt(b[1]) - ox);
  }
  if (s === 0n) return "degenerate";
  for (let i = 0; i < n; i++)
    for (let j = i + 1; j < n; j++) {
      if (adjacent(i, j, n)) continue;
      if (segmentsIntersect(v[i], v[(i + 1) % n], v[j], v[(j + 1) % n])) return "selfX";
    }
  return null;
}

export const validateRing = (ring: LonLat[]) => validateE7(canonicalE7(ring));

export type Relation = "inside" | "boundary" | "outside";

/** Firmware classifyPointInGeofencePolygon for an already valid polygon. */
export function classifyE7(v: E7[], p: E7): Relation {
  let la0 = v[0][0], la1 = v[0][0], lo0 = v[0][1], lo1 = v[0][1];
  for (const c of v) { la0 = Math.min(la0, c[0]); la1 = Math.max(la1, c[0]); lo0 = Math.min(lo0, c[1]); lo1 = Math.max(lo1, c[1]); }
  if (p[0] < la0 || p[0] > la1 || p[1] < lo0 || p[1] > lo1) return "outside";
  const n = v.length;
  for (let i = 0; i < n; i++) if (onSegment(v[i], v[(i + 1) % n], p)) return "boundary";
  let w = 0;
  for (let i = 0; i < n; i++) {
    const a = v[i], b = v[(i + 1) % n];
    if (a[0] <= p[0]) { if (b[0] > p[0] && cross(a, b, p) > 0n) w++; }
    else if (b[0] <= p[0] && cross(a, b, p) < 0n) w--;
  }
  return w === 0 ? "outside" : "inside";
}

// ----------------------------------------------------------- the set ---
export interface SetCheck {
  active: Zone[];
  total: number;          // active vertices (collar capacity counts these)
  errors: string[];       // plain Turkish, ready to show
  ok: boolean;
}

export const ERROR_TEXT: Record<ZoneError, string> = {
  tooFew: "en az 3 köşe gerekli",
  tooMany: `bir alanda en fazla ${FENCE.maxPolygonVertices} köşe olabilir`,
  range: "koordinat geçersiz",
  span: "alan 10 dereceden geniş olamaz",
  dup: "iki komşu köşe aynı noktada",
  degenerate: "alanın büyüklüğü sıfır (köşeler tek hat üzerinde)",
  selfX: "kenarlar birbirini kesiyor",
};

export function checkSet(zones: Zone[]): SetCheck {
  const active = zones.filter((z) => z.active);
  const errors: string[] = [];
  let total = 0;
  for (const z of active) {
    const v = canonicalE7(z.ring);
    total += v.length;
    const e = validateE7(v);
    if (e) errors.push(`${z.name}: ${ERROR_TEXT[e]}.`);
  }
  if (active.length > FENCE.maxAreas) errors.push(`En fazla ${FENCE.maxAreas} alan açık olabilir (şu an ${active.length}).`);
  if (total > FENCE.maxTotalVertices) errors.push(`Açık alanlarda toplam ${total} köşe var; tasma en fazla ${FENCE.maxTotalVertices} köşe taşır.`);
  return { active, total, errors, ok: errors.length === 0 };
}

/** Vertices left for one zone being edited (others' active vertices excluded). */
export function roomFor(zones: Zone[], editingId: string | null, willBeActive: boolean): number {
  if (!willBeActive) return FENCE.maxPolygonVertices;
  const used = zones.filter((z) => z.active && z.id !== editingId).reduce((s, z) => s + canonicalE7(z.ring).length, 0);
  return Math.max(0, Math.min(FENCE.maxPolygonVertices, FENCE.maxTotalVertices - used));
}

/** Compiled active, valid polygons for fast repeated point tests. */
export interface Fence { polys: { id: string; name: string; v: E7[] }[]; }

export function compileFence(zones: Zone[]): Fence {
  const polys: Fence["polys"] = [];
  for (const z of zones) {
    if (!z.active) continue;
    const v = canonicalE7(z.ring);
    if (!validateE7(v)) polys.push({ id: z.id, name: z.name, v });
  }
  return { polys };
}

/** The zone holding the point (boundary counts as inside), or null = outside all. */
export function zoneAt(f: Fence, p: LatLon): { id: string; name: string } | null {
  const q = toE7([p.lon, p.lat]);
  if (!inRange(q)) return null;
  for (const z of f.polys) if (classifyE7(z.v, q) !== "outside") return { id: z.id, name: z.name };
  return null;
}

// ------------------------------------------------- outside, confirmed ---
export interface Located extends LatLon { time: string | null; hdop: number | null; }

/** Accepted for a boundary decision: a timed fix that is not obviously poor. */
const accepted = (p: Located) => p.time !== null && (p.hdop === null || p.hdop <= 5);

export interface Episode { from: number; to: number | null; fromIdx: number; }

/**
 * Confirmed outside periods over a record list (oldest first). State changes
 * need the newest accepted record on the new side and at least 2 of the last
 * 3 accepted records on it; a single stray fix never opens or closes one.
 */
export function outsideEpisodes(points: Located[], f: Fence, time: (p: Located) => number): Episode[] {
  if (!f.polys.length) return [];
  const out: Episode[] = [];
  const win: { o: boolean; t: number; i: number }[] = [];
  let state: "in" | "out" | null = null;
  let open: Episode | null = null;
  points.forEach((p, i) => {
    if (!accepted(p)) return;
    const t = time(p);
    if (!Number.isFinite(t)) return;
    win.push({ o: zoneAt(f, p) === null, t, i });
    if (win.length > 3) win.shift();
    const outs = win.filter((w) => w.o).length, ins = win.length - outs;
    const newest = win[win.length - 1];
    if (newest.o && outs >= 2 && state !== "out") {
      const first = win.find((w) => w.o)!;
      state = "out";
      open = { from: first.t, to: null, fromIdx: first.i };
      out.push(open);
    } else if (!newest.o && ins >= 2 && state !== "in") {
      if (open) open.to = win.find((w) => !w.o)!.t;
      open = null;
      state = "in";
    }
  });
  return out;
}

// ------------------------------------------------------------ measure ---
function local(ring: LonLat[]): [number, number][] {
  const lat0 = ring[0][1], lon0 = ring[0][0];
  const kx = 111_320 * Math.cos((lat0 * Math.PI) / 180), ky = 110_540;
  return ring.map(([lon, lat]) => [(lon - lon0) * kx, (lat - lat0) * ky]);
}

/** Square metres (field-scale planar). */
export function areaM2(ring: LonLat[]): number {
  if (ring.length < 3) return 0;
  const p = local(ring);
  let s = 0;
  for (let i = 0; i < p.length; i++) { const a = p[i], b = p[(i + 1) % p.length]; s += a[0] * b[1] - b[0] * a[1]; }
  return Math.abs(s) / 2;
}

export function edgeM(a: LonLat, b: LonLat): number {
  return distanceM({ lat: a[1], lon: a[0] }, { lat: b[1], lon: b[0] });
}

export function perimeterM(ring: LonLat[], closed = true): number {
  let s = 0;
  for (let i = 1; i < ring.length; i++) s += edgeM(ring[i - 1], ring[i]);
  if (closed && ring.length > 2) s += edgeM(ring[ring.length - 1], ring[0]);
  return s;
}

export function centroid(ring: LonLat[]): LonLat {
  let x = 0, y = 0;
  for (const [lon, lat] of ring) { x += lon; y += lat; }
  return [x / ring.length, y / ring.length];
}

export function bounds(ring: LonLat[]): [LonLat, LonLat] {
  let a = Infinity, b = Infinity, c = -Infinity, d = -Infinity;
  for (const [lon, lat] of ring) { a = Math.min(a, lon); b = Math.min(b, lat); c = Math.max(c, lon); d = Math.max(d, lat); }
  return [[a, b], [c, d]];
}

// ----------------------------------------------------------- simplify ---
function rdp(pts: [number, number][], idx: number[], tol: number): number[] {
  if (idx.length <= 2) return idx;
  const a = pts[idx[0]], b = pts[idx[idx.length - 1]];
  const dx = b[0] - a[0], dy = b[1] - a[1], L = Math.hypot(dx, dy) || 1;
  let best = -1, far = 0;
  for (let k = 1; k < idx.length - 1; k++) {
    const p = pts[idx[k]];
    const d = Math.abs(dy * p[0] - dx * p[1] + b[0] * a[1] - b[1] * a[0]) / L;
    if (d > far) { far = d; best = k; }
  }
  if (far <= tol) return [idx[0], idx[idx.length - 1]];
  const left = rdp(pts, idx.slice(0, best + 1), tol), right = rdp(pts, idx.slice(best), tol);
  return [...left.slice(0, -1), ...right];
}

/**
 * Fewer corners: the smallest tolerance that brings the ring to `target`
 * vertices or below and stays valid for the collar. null if none found.
 */
export function simplifyRing(ring: LonLat[], target: number): LonLat[] | null {
  if (ring.length <= target && !validateRing(ring)) return ring;
  const p = local(ring);
  // Split at the vertex farthest from vertex 0 so both halves keep their shape.
  let far = 0, fi = 0;
  p.forEach((q, i) => { const d = Math.hypot(q[0] - p[0][0], q[1] - p[0][1]); if (d > far) { far = d; fi = i; } });
  const closed = [...p, p[0]];
  const all = closed.map((_, i) => i);
  for (let tol = 0.5; tol < 5000; tol *= 1.4) {
    const a = rdp(closed, all.slice(0, fi + 1), tol), b = rdp(closed, all.slice(fi), tol);
    const keep = [...a.slice(0, -1), ...b].filter((i) => i < ring.length);
    const uniq = [...new Set(keep)].sort((x, y) => x - y);
    if (uniq.length < 3) return null;
    if (uniq.length <= target) {
      const out = uniq.map((i) => ring[i]);
      if (!validateRing(out)) return out;
    }
  }
  return null;
}

// ------------------------------------------------------------ GeoJSON ---
export function toGeoJSON(zones: Zone[]): string {
  return JSON.stringify({
    type: "FeatureCollection",
    features: zones.map((z) => ({
      type: "Feature",
      properties: { id: z.id, name: z.name, active: z.active, area_ha: Math.round(areaM2(z.ring) / 100) / 100 },
      geometry: { type: "Polygon", coordinates: [[...z.ring, z.ring[0]]] },
    })),
  }, null, 2);
}

export interface Imported { zones: Zone[]; skipped: string[]; }

/** Outer rings of Polygon / MultiPolygon features; holes are not supported by the collar. */
export function fromGeoJSON(text: string, now: number, newId: () => string): Imported {
  const data = JSON.parse(text) as { type?: string; features?: unknown[]; geometry?: unknown; properties?: Record<string, unknown> };
  if (!data || typeof data !== "object" || Array.isArray(data)) throw new Error("GeoJSON nesnesi geçersiz.");
  if (data.type === "FeatureCollection" && !Array.isArray(data.features)) throw new Error("GeoJSON features listesi geçersiz.");
  const feats: { geometry?: { type?: string; coordinates?: unknown }; properties?: Record<string, unknown> | null }[] =
    data.type === "FeatureCollection" ? (data.features ?? []) as never
      : data.type === "Feature" ? [data as never] : [{ geometry: data as never, properties: {} }];
  const zones: Zone[] = [], skipped: string[] = [];
  feats.forEach((f, i) => {
    const g = f.geometry, props = f.properties ?? {};
    const base = String(props.name ?? props.ad ?? props.Name ?? props.title ?? `Alan ${i + 1}`).trim().slice(0, 40) || `Alan ${i + 1}`;
    if (g?.type !== "Polygon" && g?.type !== "MultiPolygon") {
      skipped.push(`${base}: alan (çokgen) değil`); return;
    }
    // The firmware cannot represent GeoJSON holes. Silently dropping them
    // would turn excluded land into allowed pasture. Reject the feature.
    const polygons = g.type === "Polygon" ? [g.coordinates] : g.coordinates;
    if (!Array.isArray(polygons) || !polygons.length || polygons.some((p) =>
      !Array.isArray(p) || !p.length || p.length !== 1)) {
      skipped.push(`${base}: iç boşluklu veya bozuk çokgen desteklenmiyor`);
      return;
    }
    const rings: unknown[] = polygons.map((p) => (p as unknown[])[0]);
    if (!rings.length) { skipped.push(`${base}: alan (çokgen) değil`); return; }
    rings.forEach((r, k) => {
      const name = rings.length > 1 ? `${base} ${k + 1}` : base;
      const ring = (Array.isArray(r) ? r : []).filter((c): c is number[] => Array.isArray(c) && c.length === 2 && c.every((n) => typeof n === "number" && Number.isFinite(n)))
        .map((c) => [c[0], c[1]] as LonLat);
      if (ring.length > 1 && ring[0][0] === ring[ring.length - 1][0] && ring[0][1] === ring[ring.length - 1][1]) ring.pop();
      const e = validateRing(ring);
      if (e) { skipped.push(`${name}: ${ERROR_TEXT[e]}`); return; }
      zones.push({ id: newId(), name, ring, active: true, updated: now });
    });
  });
  return { zones, skipped };
}

export function newZoneId(): string {
  return `z${Date.now().toString(36)}${Math.floor(Math.random() * 1e6).toString(36)}`;
}
