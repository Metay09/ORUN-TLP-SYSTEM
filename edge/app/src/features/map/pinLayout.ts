// Screen-space marker layout for the livestock field map.
// Coordinates are CSS pixels from MapLibre.project(): NEVER change recorded GNSS positions.
export interface ScreenPin {
  id: string;
  x: number;
  y: number;
  selected?: boolean;
}
export interface PinGroup {
  ids: string[];
  x: number;
  y: number;
  selectedId: string | null;
}

const clamp = (value: number, lo: number, hi: number) => Math.max(lo, Math.min(hi, value));

/**
 * At ordinary farm zoom (14) the status ring is still readable; at field-scale
 * detail (18+) it shrinks so the actual GPS position and paddock are visible.
 * At country zoom it also shrinks; aggregation handles overlapping animals.
 */
export function pinDiameter(zoom: number, selected = false): number {
  const z = Number.isFinite(zoom) ? clamp(zoom, 9, 20) : 14;
  const base = z <= 14 ? 18 + (z - 9) * 2.4 : 30 - (z - 14) * 1.9;
  return Math.round(clamp(base, 18, 30)) + (selected ? 4 : 0);
}

export function clusterDiameter(zoom: number): number {
  return Math.round(clamp(pinDiameter(zoom) + 2, 24, 32));
}

/** Group in projected pixels, not metres or approximate longitude offsets.
 * This also works at maximum zoom when multiple collars report one position.
 * Selection is kept accessible by showing the selected pin with a +N badge.
 */
export function groupScreenPins(points: readonly ScreenPin[], zoom: number): PinGroup[] {
  if (!points.length) return [];
  const gap = Math.max(pinDiameter(zoom, true), clusterDiameter(zoom)) + 5;
  const radiusSq = gap * gap;
  const parent = points.map((_, i) => i);
  const find = (n: number): number => {
    while (parent[n] !== n) { parent[n] = parent[parent[n]]; n = parent[n]; }
    return n;
  };
  const union = (a: number, b: number) => {
    const ra = find(a), rb = find(b);
    if (ra !== rb) parent[rb] = ra;
  };

  // MapLibre screen projection is in CSS px. Distance stays meaningful across
  // map bearing/pitch and resizes; the grouping changes only on map settles.
  for (let i = 0; i < points.length; i++) {
    for (let j = i + 1; j < points.length; j++) {
      const dx = points[i].x - points[j].x, dy = points[i].y - points[j].y;
      if (dx * dx + dy * dy < radiusSq) union(i, j);
    }
  }
  const groups = new Map<number, ScreenPin[]>();
  points.forEach((point, i) => {
    const root = find(i);
    const arr = groups.get(root);
    if (arr) arr.push(point); else groups.set(root, [point]);
  });
  return [...groups.values()].map((members) => ({
    ids: members.map((p) => p.id),
    x: members.reduce((n, p) => n + p.x, 0) / members.length,
    y: members.reduce((n, p) => n + p.y, 0) / members.length,
    selectedId: members.find((p) => p.selected)?.id ?? null,
  }));
}
