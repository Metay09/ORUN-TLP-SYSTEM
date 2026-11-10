// Geometry for map display only. Original observations and timestamps are
// never modified: storage, distance totals and playback retain real fixes.
import { distanceM } from "../../app/geo.ts";
import { agingLimit, epochMs, periodMs } from "../../app/herd.ts";
import type { Observation } from "../../app/model.ts";

const MAX_PLAUSIBLE_SPEED_MPS = 4; // consistent with geo.pathLengthM
const DRAW_TOLERANCE_M = 10;        // display tolerance, not GPS accuracy

export interface ReadableTrail {
  runs: Observation[][];
  breaks: number;
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

/** Repeated fixes are ignored; implausible jumps and stale gaps split lines. */
export function readableTrail(points: Observation[]): ReadableTrail {
  const ordered = [...points].filter(valid).sort((a, b) => epochMs(a.time) - epochMs(b.time));
  const runs: Observation[][] = [];
  let run: Observation[] = [], breaks = 0;
  for (const point of ordered) {
    const prev = run[run.length - 1];
    if (prev) {
      const elapsed = epochMs(point.time) - epochMs(prev.time);
      if (elapsed <= 0) continue;
      const speed = distanceM(prev, point) / (elapsed / 1000);
      if (elapsed > agingLimit(periodMs(prev.period_s)) || speed > MAX_PLAUSIBLE_SPEED_MPS) {
        runs.push(simplify(run));
        run = [];
        breaks++;
      }
    }
    run.push(point);
  }
  if (run.length) runs.push(simplify(run));
  return { runs, breaks };
}
