// Pure gesture math for the ORUN map bottom sheet. Visual layout stays the
// same: compact / half / full. No toggle button or accidental tap cycling.
export type Snap = "peek" | "half" | "full";
const ORDER: readonly Snap[] = ["peek", "half", "full"];

export function snapHeight(snap: Snap, total: number, peek: number, half = 0.46): number {
  // Keep all three stops inside the map, even on small landscape screens.
  const maximum = Math.max(0, Math.round(total - 72));
  const compact = Math.min(Math.max(0, peek), maximum);
  const middle = Math.min(maximum, Math.max(compact, Math.round(total * half)));
  if (snap === "peek") return compact;
  if (snap === "half") return middle;
  return maximum;
}

export function clampSheetHeight(value: number, total: number, peek: number, half = 0.46): number {
  return Math.min(snapHeight("full", total, peek, half),
    Math.max(snapHeight("peek", total, peek, half), value));
}

/** Positive dy/velocity means finger travelling UP (sheet grows).
 * A deliberate 28px move or a quick 12px flick advances one stop minimum,
 * even if nearest-distance snapping would pull a compact panel back down.
 * A long drag can still skip a stop; tiny accidental movements do nothing.
 */
export function settleSheet(
  from: Snap, dy: number, releaseHeight: number, total: number,
  peek: number, half = 0.46, velocity = 0,
): Snap {
  if (!Number.isFinite(dy) || !Number.isFinite(releaseHeight)) return from;
  const moved = Math.abs(dy) >= 28;
  const flicked = Math.abs(dy) >= 12 && Math.abs(velocity) >= 0.38;
  if (!moved && !flicked) return from;

  const stops = ORDER.map((snap) => snapHeight(snap, total, peek, half));
  // If viewport is very short, some stops can coincide. Do not jump to an
  // invisible next state in that case.
  const i = ORDER.indexOf(from);
  const next = dy > 0 ? ORDER.slice(i + 1).find((snap) => stops[ORDER.indexOf(snap)] > stops[i])
    : [...ORDER.slice(0, i)].reverse().find((snap) => stops[ORDER.indexOf(snap)] < stops[i]);
  if (!next) return from;

  let nearest: Snap = from;
  for (const candidate of ORDER) {
    if (Math.abs(stops[ORDER.indexOf(candidate)] - releaseHeight) <
        Math.abs(stops[ORDER.indexOf(nearest)] - releaseHeight)) nearest = candidate;
  }
  if (dy > 0) return ORDER[Math.max(ORDER.indexOf(next), ORDER.indexOf(nearest))];
  return ORDER[Math.min(ORDER.indexOf(next), ORDER.indexOf(nearest))];
}
