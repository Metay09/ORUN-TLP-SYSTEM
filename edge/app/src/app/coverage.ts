// Coverage map (scope §4): 5 m cells, averaged over every measurement in the
// cell. Only "live" records count as a signal sample for their position —
// a record replayed later from the collar's memory was heard somewhere else.
// A cell with only "cut" records (measured while nobody heard the collar)
// is "none" (no link at that moment). A cell with no records is simply not
// drawn: "no data" is not "no coverage".
//
// Class from the LoRa margin: SNR when the radio reported it (SF11 decodes
// down to about -17.5 dB), else RSSI.
import { Grid } from "./geo.ts";
import type { Observation } from "./model.ts";

export type CoverClass = "strong" | "medium" | "weak" | "none";

export interface Cell {
  i: number; j: number;
  n: number;              // live samples
  cut: number;            // records measured with no link
  rssi: number | null;    // mean
  snr: number | null;     // mean
  cls: CoverClass;
  ring: [number, number][];
}

export const CELL_M = 5;

export function classify(rssi: number | null, snr: number | null): CoverClass {
  if (snr !== null) return snr >= 0 ? "strong" : snr >= -10 ? "medium" : "weak";
  if (rssi !== null) return rssi >= -105 ? "strong" : rssi >= -120 ? "medium" : "weak";
  return "weak";
}

export interface Coverage { refLat: number; size: number; cells: Cell[]; }

export function coverageCells(points: Observation[], size = CELL_M): Coverage {
  const sample = points.find((p) => p.link === "live" || p.link === "cut");
  if (!sample) return { refLat: 0, size, cells: [] };
  const refLat = Math.round(sample.lat * 10) / 10;
  const grid = new Grid(refLat, size);
  const acc = new Map<string, { i: number; j: number; n: number; cut: number; rs: number; rn: number; ss: number; sn: number }>();
  for (const p of points) {
    if (p.link !== "live" && p.link !== "cut") continue;
    const [i, j] = grid.cell(p);
    const k = `${i}:${j}`;
    let c = acc.get(k);
    if (!c) { c = { i, j, n: 0, cut: 0, rs: 0, rn: 0, ss: 0, sn: 0 }; acc.set(k, c); }
    if (p.link === "cut") { c.cut++; continue; }
    c.n++;
    if (p.rssi !== null) { c.rs += p.rssi; c.rn++; }
    if (p.snr !== null) { c.ss += p.snr; c.sn++; }
  }
  const out: Cell[] = [];
  for (const c of acc.values()) {
    const rssi = c.rn ? c.rs / c.rn : null, snr = c.sn ? c.ss / c.sn : null;
    out.push({
      i: c.i, j: c.j, n: c.n, cut: c.cut, rssi, snr,
      cls: c.n ? classify(rssi, snr) : "none",
      ring: grid.ring(c.i, c.j),
    });
  }
  return { refLat, size, cells: out };
}

export function coverageSummary(cells: Cell[]): { cells: number; strong: number; weak: number } {
  const n = cells.length;
  if (!n) return { cells: 0, strong: 0, weak: 0 };
  const strong = cells.filter((c) => c.cls === "strong").length;
  const weak = cells.filter((c) => c.cls === "weak" || c.cls === "none").length;
  return { cells: n, strong: Math.round((strong / n) * 100), weak: Math.round((weak / n) * 100) };
}

/** Coverage class at a position (the cell it falls in), if measured. */
export function coverageAt(cov: Coverage, lat: number, lon: number): CoverClass | null {
  if (!cov.cells.length) return null;
  const [i, j] = new Grid(cov.refLat, cov.size).cell({ lat, lon });
  return cov.cells.find((c) => c.i === i && c.j === j)?.cls ?? null;
}
