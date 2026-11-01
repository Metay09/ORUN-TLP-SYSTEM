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
  const gapSq = gap * gap;
  const groups: ScreenPin[][] = [];

  // Bounded complete-link grouping: avoid the "chain effect" where dozens
  // of 35px-spaced animals across an entire pasture become one giant cluster.
  // Every group member stays within the screen-space collision radius of
  // every other member. Keep the input immutable.
  for (const point of points) {
    const group = groups.find((members) => members.every((p) => {
      const dx = point.x - p.x, dy = point.y - p.y;
      return dx * dx + dy * dy < gapSq;
    }));
    if (group) group.push(point);
    else groups.push([point]);
  }
  return groups.map((members) => {
    // A selected animal is never displaced by averaging other GPS fixes.
    const selected = members.find((p) => p.selected);
    return {
      ids: members.map((p) => p.id),
      x: selected?.x ?? members.reduce((n, p) => n + p.x, 0) / members.length,
      y: selected?.y ?? members.reduce((n, p) => n + p.y, 0) / members.length,
      selectedId: selected?.id ?? null,
    };
  });
}
