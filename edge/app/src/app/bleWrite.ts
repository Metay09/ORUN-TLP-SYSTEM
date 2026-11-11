// 4c-1b: tasmaya Bluetooth'tan ayar yazma. Yalnız hesaba girmiş telefon yazar:
// sunucu her tasma için bir yazma anahtarı verir (tasmanın kökünden türetilir,
// kök değil), telefon onu saklar, sahada internetsiz çalışır. Her yazmadan önce
// tasma yeni, tek kullanımlık bir soru (challenge) verir; telefon isteği o
// soruyla imzalar, tasma cevabını aynı soruyla imzalar. "Tasmaya işlendi"
// yalnız tasmanın imzalı cevabıyla (firmware ble_write_auth.h ile aynı baytlar).
import { concat, equal, fromHex, hex } from "../core/bytes.ts";
import { hmacSha256, sha256 } from "../core/crypto.ts";
import { ERROR_TYPE, type AppMessage } from "./bleAppFrames.ts";
import { CLEAR_SUMMARY, DeviceRejected, polygonsOf, summary, type FenceSummary } from "./fenceUsb.ts";
import { outcomeResult, SETTINGS, type ServiceChoice, type SettingWriter } from "./trackerSettings.ts";
import { FENCE, type E7, type Zone } from "./zones.ts";

export const WRITE = {
  challenge: 0x10, write: 0x11, stage: 0x12, challengeAnswer: 0x90, writeAnswer: 0x91, stageAnswer: 0x92,
} as const;
/** Ayar işlemleri tek ayar tablosundan (SETTINGS); alan listesi kendi sahibinde. */
export const OP = {
  interval: SETTINGS.interval.id, services: SETTINGS.services.id, bleWindow: SETTINGS.bleWindow.id,
  fence: 4, fenceClear: 5,
} as const;
/** 4c-2 alan listesi aşamaları (firmware ble_application_transport.h). */
export const STAGE = { begin: 1, vertices: 2, nextArea: 3, abort: 4 } as const;
const STAGE_MAX_POINTS = 5;
export type Op = typeof OP[keyof typeof OP];

const enc = new TextEncoder();
const REQ = enc.encode("ORUN-BLE-WRITE-REQ-v1");
const RES = enc.encode("ORUN-BLE-WRITE-RES-v1");

/** ConfigMutationOutcome (firmware config_mutation.h), same words as USB. */
export const OUTCOME = ["APPLIED", "UNCHANGED", "INVALID", "ACCESS_DENIED", "MAINTENANCE", "UNAVAILABLE", "OUTCOME_UNKNOWN", "STALE"] as const;
export type Outcome = typeof OUTCOME[number];

const be64 = (idHex: string) => fromHex(idHex.toUpperCase().padStart(16, "0"));
const be32 = (n: number) => Uint8Array.from([(n >>> 24) & 0xff, (n >>> 16) & 0xff, (n >>> 8) & 0xff, n & 0xff]);
const le32 = (n: number) => Uint8Array.from([n & 0xff, (n >>> 8) & 0xff, (n >>> 16) & 0xff, (n >>> 24) & 0xff]);
const getLe32 = (p: Uint8Array, o: number) => (p[o] | (p[o + 1] << 8) | (p[o + 2] << 16) | (p[o + 3] << 24)) >>> 0;

export interface Challenge { device: string; epoch: number; challenge: Uint8Array; }

/** The tasma's challenge answer: device id (BE), key epoch (LE), 16 bytes. */
export function parseChallenge(m: AppMessage): Challenge {
  if (m.type === ERROR_TYPE) throw writeRefused(m.payload[0]);
  if (m.type !== WRITE.challengeAnswer || m.payload.length !== 29 || m.payload[0] !== 0)
    throw new Error("Tasmadan beklenmedik yanıt.");
  return { device: hex(m.payload.subarray(1, 9)), epoch: getLe32(m.payload, 9), challenge: m.payload.slice(13, 29) };
}

function tag(key: Uint8Array, domain: Uint8Array, c: Challenge, body: Uint8Array): Uint8Array {
  return hmacSha256(key, concat(domain, be64(c.device), be32(c.epoch), c.challenge, body)).slice(0, 16);
}

/**
 * Write request payload: op, value LE32, tag over those 5 bytes. An area list
 * commit also MACs the digest of the staged list (`extra`, not sent).
 */
export function writeRequest(key: Uint8Array, c: Challenge, op: Op, value: number, extra: Uint8Array = new Uint8Array(0)): Uint8Array {
  if (!Number.isInteger(value) || value < 0 || value > 0xffffffff) throw new Error("Geçersiz değer.");
  const body = concat(Uint8Array.of(op), le32(value));
  return concat(body, tag(key, REQ, c, concat(body, extra)));
}

/** SHA-256 of the list as the tasma stages it: areas, then per area n and n × (lat, lon) BE32. */
export function stagedDigest(polys: E7[][]): Uint8Array {
  const parts: number[] = [polys.length];
  for (const p of polys) {
    parts.push(p.length);
    for (const [la, lo] of p) for (const v of [la, lo]) parts.push((v >>> 24) & 0xff, (v >>> 16) & 0xff, (v >>> 8) & 0xff, v & 0xff);
  }
  return sha256(Uint8Array.from(parts));
}

export interface WriteResult {
  outcome: Outcome; op: number; intervalS: number; bleWindowS: number;
  serviceMode: number; services: number; revision: number;
}

/** The answer bytes, only if the tasma itself signed them. */
function signedAnswer(key: Uint8Array, c: Challenge, m: AppMessage, op: Op): Uint8Array {
  if (m.type === ERROR_TYPE) throw writeRefused(m.payload[0]);
  const p = m.payload;
  if (m.type !== WRITE.writeAnswer || p.length !== 30) throw new Error("Tasmadan beklenmedik yanıt.");
  if (!equal(tag(key, RES, c, p.subarray(0, 14)), p.subarray(14, 30)))
    throw new Error("Tasmanın cevabı doğrulanamadı; yazıldığını varsayma.");
  if (p[1] !== op) throw new Error("Tasmadan beklenmedik yanıt.");
  return p;
}

/** The tasma's signed answer; throws unless the tasma itself signed it. */
export function parseResult(key: Uint8Array, c: Challenge, m: AppMessage, op: Op): WriteResult {
  const p = signedAnswer(key, c, m, op);
  return {
    outcome: OUTCOME[p[0]] ?? "UNAVAILABLE", op: p[1], intervalS: getLe32(p, 2), bleWindowS: p[6] | (p[7] << 8),
    serviceMode: p[8], services: p[9], revision: getLe32(p, 10),
  };
}

const REFUSED: Record<number, string> = {
  1: "Bu cihaz Bluetooth'tan ayar almıyor (yazılımı eski olabilir).",
  2: "Tasma şu an meşgul; birkaç saniye sonra tekrar dene.",
  3: "Tasma yazmayı kabul etmedi (anahtar ya da soru geçersiz). Bağlantıyı yenileyip tekrar dene.",
  4: "Tasma şu an hazır değil.",
};
export class WriteRefused extends Error {}
function writeRefused(code: number): Error {
  return new WriteRefused(REFUSED[code] ?? "Tasma yazmayı reddetti.");
}

/** Firmware service intent: mode EXPLICIT (1), bits T=1 R=2 (A only on a gateway). */
export function servicesValue(s: ServiceChoice): number {
  return 1 | (((s.tracking ? 1 : 0) | (s.relay ? 2 : 0)) << 8);
}

/** One request, one answer (the BLE link). */
export type Exchange = (type: number, payload: Uint8Array, waitMs?: number) => Promise<AppMessage>;
export type KeyFor = (device: string, epoch: number) => Uint8Array | null;

export class NoWriteKey extends Error {}

/** Asks a fresh challenge, writes one value, returns the tasma's signed answer. */
export async function bleWrite(exchange: Exchange, keyFor: KeyFor, device: string, op: Op, value: number): Promise<WriteResult> {
  const c = parseChallenge(await exchange(WRITE.challenge, new Uint8Array(0)));
  if (c.device !== device.toUpperCase()) throw new Error("Bağlı cihaz beklenen tasma değil.");
  const key = keyFor(c.device, c.epoch);
  if (!key) throw new NoWriteKey("Bu telefonda bu tasmanın yazma anahtarı yok. İnternete bağlanınca hesaptan kendiliğinden gelir; şimdilik USB ile yaz.");
  const request = writeRequest(key, c, op, value);
  let answer: AppMessage;
  try {
    answer = await exchange(WRITE.write, request, 15_000);
  } catch {
    throw new Error("Tasmadan yanıt gelmedi; ayarın yazıldığını varsayma, durumu yeniden oku.");
  }
  return parseResult(key, c, answer, op);
}

/** The same settings order as USB, over Bluetooth (trackerSettings.writeWaiting). */
export function bleSettingWriter(exchange: Exchange, keyFor: KeyFor, device: string): SettingWriter {
  const run = async (op: Op, value: number) => outcomeResult((await bleWrite(exchange, keyFor, device, op, value)).outcome);
  return {
    interval: (s) => run(OP.interval, s),
    bleWindow: (s) => run(OP.bleWindow, s),
    services: (c) => run(OP.services, servicesValue(c)),
  };
}

// ---- 4c-2: alan listesi --------------------------------------------------

/** GeofenceResourceState (firmware geofence_store.h). */
const FENCE_STATE = ["unavailable", "clear", "configured"] as const;

export interface FenceResult {
  outcome: Outcome; state: typeof FENCE_STATE[number] | "unknown"; areas: number; vertices: number; revision: number;
}

export function parseFenceResult(key: Uint8Array, c: Challenge, m: AppMessage, op: Op = OP.fence): FenceResult {
  const p = signedAnswer(key, c, m, op);
  return {
    outcome: OUTCOME[p[0]] ?? "UNAVAILABLE", state: FENCE_STATE[p[2]] ?? "unknown",
    areas: p[3], vertices: p[4] | (p[5] << 8), revision: getLe32(p, 10),
  };
}

const STAGE_WHY = ["", "liste başlamadı", "en fazla 64 köşe", "boş alan", "en fazla 8 alan", "biçim hatası"];

async function stageStep(exchange: Exchange, payload: Uint8Array, areas: number, vertices: number): Promise<void> {
  const m = await exchange(WRITE.stage, payload);
  if (m.type === ERROR_TYPE) throw writeRefused(m.payload[0]);
  const p = m.payload;
  if (m.type !== WRITE.stageAnswer || p.length !== 4) throw new Error("Tasmadan beklenmedik yanıt.");
  if (p[0] !== 0) throw new DeviceRejected(`Tasma alan listesini reddetti: ${STAGE_WHY[p[0]] ?? p[0]}.`);
  if (p[1] !== areas || (p[2] | (p[3] << 8)) !== vertices) throw new Error("Tasmanın aldığı köşe sayısı tutmuyor.");
}

function pointsMessage(points: E7[]): Uint8Array {
  const out = new Uint8Array(2 + 8 * points.length);
  out[0] = STAGE.vertices; out[1] = points.length;
  points.forEach(([la, lo], i) => { out.set(le32(la >>> 0), 2 + 8 * i); out.set(le32(lo >>> 0), 6 + 8 * i); });
  return out;
}

/**
 * Writes the whole list (it replaces the tasma's), exactly like USB writeFence:
 * staged in pieces, then one signed commit over the digest of what was staged.
 * Success only on the tasma's signed answer.
 */
export async function bleWriteFence(exchange: Exchange, keyFor: KeyFor, device: string, zones: Zone[]): Promise<FenceSummary> {
  const polys = polygonsOf(zones);
  const sum = summary(polys);
  if (!sum.areas) throw new Error("Bu tasmaya gidecek alan yok.");
  if (sum.areas > FENCE.maxAreas || sum.vertices > FENCE.maxTotalVertices)
    throw new Error(`Tasma en fazla ${FENCE.maxAreas} alan ve ${FENCE.maxTotalVertices} köşe alır.`);
  let committed = false;
  try {
    await stageStep(exchange, Uint8Array.of(STAGE.begin), 0, 0);
    let total = 0;
    for (let a = 0; a < polys.length; a++) {
      if (a) await stageStep(exchange, Uint8Array.of(STAGE.nextArea), a, total);
      for (let i = 0; i < polys[a].length; i += STAGE_MAX_POINTS) {
        const chunk = polys[a].slice(i, i + STAGE_MAX_POINTS);
        total += chunk.length;
        await stageStep(exchange, pointsMessage(chunk), a + 1, total);
      }
    }
    const c = parseChallenge(await exchange(WRITE.challenge, new Uint8Array(0)));
    if (c.device !== device.toUpperCase()) throw new Error("Bağlı cihaz beklenen tasma değil.");
    const key = keyFor(c.device, c.epoch);
    if (!key) throw new NoWriteKey("Bu telefonda bu tasmanın yazma anahtarı yok. İnternete bağlanınca hesaptan kendiliğinden gelir; şimdilik USB ile yaz.");
    const request = writeRequest(key, c, OP.fence, sum.areas | (sum.vertices << 8), stagedDigest(polys));
    committed = true;
    let answer: AppMessage;
    try {
      answer = await exchange(WRITE.write, request, 25_000);
    } catch {
      throw new Error("Tasmadan onay gelmedi; alanların yazıldığını varsayma.");
    }
    const r = parseFenceResult(key, c, answer);
    outcomeResult(r.outcome);
    if (r.state !== "configured" || r.areas !== sum.areas || r.vertices !== sum.vertices)
      throw new Error(`Tasma alanları kabul ettiğini doğrulamadı (${r.areas} alan, ${r.vertices} köşe).`);
    return sum;
  } catch (e) {
    if (!committed) { try { await exchange(WRITE.stage, Uint8Array.of(STAGE.abort)); } catch { /* gone */ } }
    throw e;
  }
}

/**
 * Removes every zone from the tasma (owner 2026-11-11): the explicit op 5,
 * value 0, nothing staged; same as USB APP FENCE CLEAR. Success only on the
 * tasma's signed answer saying CLEAR with 0 areas. Needs tasma firmware with
 * op 5; an older one answers signed INVALID and nothing is removed.
 */
export async function bleClearFence(exchange: Exchange, keyFor: KeyFor, device: string): Promise<FenceSummary> {
  const c = parseChallenge(await exchange(WRITE.challenge, new Uint8Array(0)));
  if (c.device !== device.toUpperCase()) throw new Error("Bağlı cihaz beklenen tasma değil.");
  const key = keyFor(c.device, c.epoch);
  if (!key) throw new NoWriteKey("Bu telefonda bu tasmanın yazma anahtarı yok. İnternete bağlanınca hesaptan kendiliğinden gelir; şimdilik USB ile sil.");
  let answer: AppMessage;
  try {
    answer = await exchange(WRITE.write, writeRequest(key, c, OP.fenceClear, 0), 25_000);
  } catch {
    throw new Error("Tasmadan onay gelmedi; alanların silindiğini varsayma.");
  }
  const r = parseFenceResult(key, c, answer, OP.fenceClear);
  // A tasma without op 5 answers it like an unknown setting: signed INVALID, nothing removed.
  if (r.outcome === "INVALID")
    throw new DeviceRejected("Tasma Bluetooth'tan alan silmeyi kabul etmedi (yazılımı eskiyse güncelle ya da USB ile sil).");
  outcomeResult(r.outcome);
  if (r.state !== "clear" || r.areas !== 0) throw new Error("Tasma alanları sildiğini doğrulamadı.");
  return CLEAR_SUMMARY;
}
