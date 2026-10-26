// Small geodesy helpers (spherical Earth; metre accuracy is plenty for a
// pasture). Pure, no DOM.

export interface LatLon { lat: number; lon: number; }

const R = 6371000;
const rad = (d: number) => (d * Math.PI) / 180;
const deg = (r: number) => (r * 180) / Math.PI;

export function distanceM(a: LatLon, b: LatLon): number {
  const dLat = rad(b.lat - a.lat), dLon = rad(b.lon - a.lon);
  const h = Math.sin(dLat / 2) ** 2 + Math.cos(rad(a.lat)) * Math.cos(rad(b.lat)) * Math.sin(dLon / 2) ** 2;
  return 2 * R * Math.asin(Math.min(1, Math.sqrt(h)));
}

/** Initial bearing a -> b, degrees clockwise from true north. */
export function bearingDeg(a: LatLon, b: LatLon): number {
  const y = Math.sin(rad(b.lon - a.lon)) * Math.cos(rad(b.lat));
  const x = Math.cos(rad(a.lat)) * Math.sin(rad(b.lat)) -
    Math.sin(rad(a.lat)) * Math.cos(rad(b.lat)) * Math.cos(rad(b.lon - a.lon));
  return (deg(Math.atan2(y, x)) + 360) % 360;
}

/** Sum of straight segments; ignores single jumps faster than `maxSpeed` m/s. */
export function pathLengthM(points: (LatLon & { t: number })[], maxSpeed = 4): number {
  let total = 0;
  for (let i = 1; i < points.length; i++) {
    const d = distanceM(points[i - 1], points[i]);
    const dt = Math.max(1, (points[i].t - points[i - 1].t) / 1000);
    if (d / dt <= maxSpeed) total += d;
  }
  return total;
}

// Local metric grid (equirectangular around a reference latitude). Used for
// the 5 m coverage cells: cell (i, j) is [i*size, (i+1)*size) metres east and
// [j*size, (j+1)*size) metres north of (0, 0).
export class Grid {
  private kx: number;
  private ky = 111_320;
  readonly size: number;
  constructor(refLat: number, size: number) {
    this.size = size;
    this.kx = 111_320 * Math.cos(rad(refLat));
  }
  cell(p: LatLon): [number, number] {
    return [Math.floor((p.lon * this.kx) / this.size), Math.floor((p.lat * this.ky) / this.size)];
  }
  /** Ring of the cell as [lon, lat] corners (closed). */
  ring(i: number, j: number): [number, number][] {
    const lon = (x: number) => (x * this.size) / this.kx;
    const lat = (y: number) => (y * this.size) / this.ky;
    return [[lon(i), lat(j)], [lon(i + 1), lat(j)], [lon(i + 1), lat(j + 1)], [lon(i), lat(j + 1)], [lon(i), lat(j)]];
  }
}

export interface Bounds { west: number; south: number; east: number; north: number; }

export function boundsOf(points: LatLon[]): Bounds | null {
  if (!points.length) return null;
  let west = Infinity, south = Infinity, east = -Infinity, north = -Infinity;
  for (const p of points) {
    west = Math.min(west, p.lon); east = Math.max(east, p.lon);
    south = Math.min(south, p.lat); north = Math.max(north, p.lat);
  }
  return { west, south, east, north };
}
