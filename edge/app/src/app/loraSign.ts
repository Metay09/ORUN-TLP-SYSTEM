// 6c/6d: LoRa commands built and checked on the phone, for when it bridges a
// gateway without internet (the server does the same online:
// backend/lora_command.py, backend/lora_queue.py). Same bytes as firmware
// lora_command.h; the tasma's own write key, which the phone already holds
// for Bluetooth writes. Shared vectors: test/loraImza.test.ts.
import { concat, equal, fromHex, hex } from "../core/bytes.ts";
import { hmacSha256 } from "../core/crypto.ts";
import { canonicalE7, type E7, type LonLat } from "./zones.ts";

const enc = new TextEncoder();
const CMD = enc.encode("ORUN-LORA-CMD-v1");
const RES = enc.encode("ORUN-LORA-RES-v1");
export const RESOURCE = { settings: 1, areas: 2 } as const;
export const AREA_CHUNK = 202;
export const OUTCOMES = ["APPLIED", "UNCHANGED", "INVALID", "ACCESS_DENIED", "MAINTENANCE", "UNAVAILABLE",
  "OUTCOME_UNKNOWN", "STALE"] as const;
export type LoraOutcome = typeof OUTCOMES[number];
export interface Token { incarnation: bigint; revision: number; }

const tag = (key: Uint8Array, domain: Uint8Array, body: Uint8Array) => hmacSha256(key, concat(domain, body)).slice(0, 16);
function be(n: bigint | number, bytes: number): Uint8Array {
  const out = new Uint8Array(bytes);
  let v = BigInt(n);
  for (let i = bytes - 1; i >= 0; i--) { out[i] = Number(v & 0xffn); v >>= 8n; }
  return out;
}
const i32 = (n: number) => be(BigInt.asUintN(32, BigInt(n)), 4);

function header(resource: number, device: string, epoch: number, cid: number, t: Token): Uint8Array {
  return concat(new Uint8Array([2, 7, 0, resource]), fromHex(device), be(epoch, 4), be(cid, 4),
                be(t.incarnation, 8), be(t.revision, 4));
}

/** One signed settings frame; entries = [setting id, value] (settings_table.cpp). */
export function settingsCommand(key: Uint8Array, device: string, epoch: number, cid: number, t: Token,
                                entries: [number, number][]): Uint8Array {
  const body = concat(header(1, device, epoch, cid, t), new Uint8Array([entries.length]),
                      ...entries.map(([id, v]) => concat(new Uint8Array([id]), be(v, 4))));
  return concat(body, tag(key, CMD, body));
}

/** Signed area list, 1..3 frames; polys = [] removes every area. */
export function areaCommand(key: Uint8Array, device: string, epoch: number, cid: number, t: Token,
                            polys: E7[][]): Uint8Array[] {
  const vertices = polys.reduce((n, p) => n + p.length, 0);
  const set = concat(new Uint8Array([polys.length, ...polys.map((p) => p.length)]),
                     ...polys.flatMap((p) => p.map(([la, lo]) => concat(i32(la), i32(lo)))));
  const parts = Math.ceil(set.length / AREA_CHUNK);
  const out: Uint8Array[] = [];
  for (let i = 0; i < parts; i++) {
    const body = concat(header(2, device, epoch, cid, t), new Uint8Array([i, parts, polys.length]), be(vertices, 2),
                        set.subarray(i * AREA_CHUNK, (i + 1) * AREA_CHUNK));
    out.push(concat(body, tag(key, CMD, body)));
  }
  return out;
}

export interface LoraResult {
  outcome: LoraOutcome; resource: number; device: string; epoch: number; commandId: number; token: Token;
  interval_s: number; ble_window_s: number; serviceMode: number; services: number;
  areaState: number; areas: number; vertices: number;
}

/** The tasma's signed answer, or null if it is not the tasma's word. */
export function parseResult(key: Uint8Array, data: Uint8Array): LoraResult | null {
  if (data.length !== 56 || data[0] !== 2 || data[1] !== 8) return null;
  if (!equal(tag(key, RES, data.subarray(0, 40)), data.subarray(40))) return null;
  const v = new DataView(data.buffer, data.byteOffset, data.byteLength);
  if (data[2] >= OUTCOMES.length) return null;
  return {
    outcome: OUTCOMES[data[2]], resource: data[3], device: hex(data.subarray(4, 12)).toUpperCase(),
    epoch: v.getUint32(12), commandId: v.getUint32(16),
    token: { incarnation: v.getBigUint64(20), revision: v.getUint32(28) },
    interval_s: data[3] === 1 ? v.getUint32(32) : 0, ble_window_s: data[3] === 1 ? v.getUint16(36) : 0,
    serviceMode: data[3] === 1 ? data[38] : 0, services: data[3] === 1 ? data[39] : 0,
    areaState: data[3] === 2 ? data[32] : 0, areas: data[3] === 2 ? data[33] : 0,
    vertices: data[3] === 2 ? v.getUint16(34) : 0,
  };
}

/** The app's setting names -> settings table rows (as backend entries_for). */
export function entriesFor(set: Record<string, number | boolean>, reported: Record<string, unknown> | null): [number, number][] {
  const out: [number, number][] = [];
  if (typeof set.interval_s === "number") out.push([1, set.interval_s]);
  if ("tracking" in set || "relay" in set) {
    const tracking = (set.tracking ?? reported?.tracking ?? true) as boolean;
    const relay = (set.relay ?? reported?.relay ?? false) as boolean;
    out.push([2, 1 | (((tracking ? 1 : 0) | (relay ? 2 : 0)) << 8)]);
  }
  if (typeof set.ble_window_s === "number") out.push([3, set.ble_window_s]);
  return out;
}

export const polysFor = (zones: { ring: LonLat[] }[]): E7[][] => zones.map((z) => canonicalE7(z.ring));
