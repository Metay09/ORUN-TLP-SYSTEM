// Geometry for map display only. Original observations and timestamps are
// never modified: storage, distance totals and playback retain real fixes.
//
// Owner decision 2026-11-16: no dashed or broken trails. Every fix is joined
// to the next in time with one solid line, so it is always visible where the
// animal went, well measured or not. Age is shown by fading (old = faint,
// new = strong), not by gaps.
import { distanceM } from "../../app/geo.ts";
import { epochMs } from "../../app/herd.ts";
import type { Observation } from "../../app/model.ts";

const MAX_PLAUSIBLE_SPEED_MPS = 4; // consistent with geo.pathLengthM
const DRAW_TOLERANCE_M = 10;        // display tolerance, not GPS accuracy

export interface ReadableTrail {
  /** Simplified line through the fixes, oldest first. */
  line: Observation[];
  /** Every fix that is drawn as a dot, oldest first. */
  fixes: Observation[];
  /** Single GPS spikes (out and straight back) left out of the drawing. */
  spikes: number;
}

function valid(p: Observation): boolean {
  return Number.isFinite(epochMs(p.time)) &&
    Number.isFinite(p.lat) && Number.isFinite(p.lon) &&
    p.lat >= -90 && p.lat <= 90 && p.lon >= -180 && p.lon <= 180;
}

function deviationM(p: Observation, a: Observation, b: Observation): number {
  const east = 111_320 * Math.cos(((a.lat + b.lat) / 2) * Math.PI / 180);
  const x = (p.lon - a.lon) * east, y = (p.lat - a.lat) * 111_320;
  const bx = (b.lon - a.lon) * east, by = (b.lat - a.lat) * 111_320;
  const lengthSquared = bx * bx + by * by;
  const u = lengthSquared ? Math.max(0, Math.min(1, (x * bx + y * by) / lengthSquared)) : 0;
  return Math.hypot(x - u * bx, y - u * by);
}

function simplify(run: Observation[]): Observation[] {
  if (run.length <= 2) return run;
  const keep = new Uint8Array(run.length);
  keep[0] = 1; keep[run.length - 1] = 1;
  const segments: [number, number][] = [[0, run.length - 1]];
  while (segments.length) {
    const [first, last] = segments.pop()!;
    let furthest = DRAW_TOLERANCE_M, index = -1;
    for (let i = first + 1; i < last; i++) {
      const dist = deviationM(run[i], run[first], run[last]);
      if (dist > furthest) { furthest = dist; index = i; }
    }
    if (index >= 0) {
      keep[index] = 1;
      segments.push([first, index], [index, last]);
    }
  }
  return run.filter((_, i) => keep[i] === 1);
}

const speedMps = (a: Observation, b: Observation): number => {
  const s = (epochMs(b.time) - epochMs(a.time)) / 1000;
  return s > 0 ? distanceM(a, b) / s : Infinity;
};

/**
 * One continuous line, oldest to newest. Repeated timestamps are ignored.
 * A single fix that jumps away and straight back (both legs faster than an
 * animal walks while the fixes around it agree) is a GPS error and is not
 * drawn. A real fast move, such as a ride in a trailer, stays in the line.
 */
export function readableTrail(points: Observation[]): ReadableTrail {
  const ordered = [...points].filter(valid).sort((a, b) => epochMs(a.time) - epochMs(b.time));
  const fixes: Observation[] = [];
  let spikes = 0;
  for (const p of ordered) {
    const prev = fixes[fixes.length - 1];
    if (prev && epochMs(p.time) <= epochMs(prev.time)) continue;
    const before = fixes[fixes.length - 2];
    // `prev` is a spike if reaching it and leaving it were both implausible
    // while going straight from `before` to `p` is an ordinary walk.
    if (before && prev && speedMps(before, prev) > MAX_PLAUSIBLE_SPEED_MPS &&
      speedMps(prev, p) > MAX_PLAUSIBLE_SPEED_MPS && speedMps(before, p) <= MAX_PLAUSIBLE_SPEED_MPS) {
      fixes.pop();
      spikes++;
    }
    fixes.push(p);
  }
  return { line: simplify(fixes), fixes, spikes };
}

/** 0 at the start of the shown period (faint), 1 at its end (strong). */
export function trailAge(timeMs: number, from: number, to: number): number {
  if (!(to > from)) return 1;
  return Math.max(0, Math.min(1, (timeMs - from) / (to - from)));
}
