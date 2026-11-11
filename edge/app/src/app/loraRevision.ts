// After a USB or Bluetooth write the tasma's settings (or areas) are at a new
// revision. The phone tells the account (and its own offline signer), so the
// next LoRa command is signed for that revision and goes in one report: no
// probe, no "STALE, sign again" round (owner 2026-11-11).
//
// Only the revision travels: the incarnation never changes in normal use, and
// the server updates a state it already learned from the tasma's own LoRa
// answer. Bluetooth revisions come only from the tasma's signed answer; USB
// ones from the cable, the same trust as every other USB result.
import type { LineConsole } from "./addTracker.ts";
import { RESOURCE } from "./loraSign.ts";

export interface StoreRevision { resource: number; revision: number; }
export type RevisionSink = (device: string, r: StoreRevision) => void;

const SET = /^APP SET id=\d+ code=\w+ .*\btoken=VALID revision=(\d+)/;
const FENCE = /^APP FENCE id=\d+ code=\w+ .*\btoken=VALID revision=(\d+)/;

/** "APP SET … token=VALID revision=N" (settings) / "APP FENCE … revision=N" (areas). */
export function revisionFromUsbLine(line: string): StoreRevision | null {
  let m = SET.exec(line);
  if (m) return Number(m[1]) > 0 ? { resource: RESOURCE.settings, revision: Number(m[1]) } : null;
  m = FENCE.exec(line);
  if (m) return Number(m[1]) > 0 ? { resource: RESOURCE.areas, revision: Number(m[1]) } : null;
  return null;
}

/** The same console, telling `sink` every revision the tasma reports on it. */
export function tapUsbRevisions(con: LineConsole, device: string, sink: RevisionSink): LineConsole {
  return {
    send: (line) => con.send(line),
    waitFor: (match, ms) => con.waitFor((line) => {
      const r = revisionFromUsbLine(line);
      if (r) sink(device.toUpperCase(), r);
      return match(line);
    }, ms),
  };
}

// Bluetooth: bleWrite.ts reports what it has verified (the tasma's signature).
let bleSink: RevisionSink | null = null;
export function onBleRevision(sink: RevisionSink | null): void { bleSink = sink; }
export function bleRevision(device: string, op: number, revision: number): void {
  if (!bleSink || !(revision > 0)) return;
  bleSink(device.toUpperCase(), { resource: op >= 4 ? RESOURCE.areas : RESOURCE.settings, revision });
}
