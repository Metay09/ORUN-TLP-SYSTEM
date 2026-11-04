// 4c-1b: tasmaya Bluetooth'tan ayar yazma. Yalnız hesaba girmiş telefon yazar:
// sunucu her tasma için bir yazma anahtarı verir (tasmanın kökünden türetilir,
// kök değil), telefon onu saklar, sahada internetsiz çalışır. Her yazmadan önce
// tasma yeni, tek kullanımlık bir soru (challenge) verir; telefon isteği o
// soruyla imzalar, tasma cevabını aynı soruyla imzalar. "Tasmaya işlendi"
// yalnız tasmanın imzalı cevabıyla (firmware ble_write_auth.h ile aynı baytlar).
import { concat, equal, fromHex, hex } from "../core/bytes.ts";
import { hmacSha256 } from "../core/crypto.ts";
import { ERROR_TYPE, type AppMessage } from "./bleAppFrames.ts";
import { outcomeResult, type ServiceChoice, type SettingWriter } from "./trackerSettings.ts";

export const WRITE = {
  challenge: 0x10, write: 0x11, challengeAnswer: 0x90, writeAnswer: 0x91,
} as const;
export const OP = { interval: 1, services: 2, bleWindow: 3 } as const;
export type Op = typeof OP[keyof typeof OP];

const enc = new TextEncoder();
const REQ = enc.encode("ORUN-BLE-WRITE-REQ-v1");
const RES = enc.encode("ORUN-BLE-WRITE-RES-v1");

/** ConfigMutationOutcome (firmware config_mutation.h), same words as USB. */
export const OUTCOME = ["APPLIED", "UNCHANGED", "INVALID", "ACCESS_DENIED", "MAINTENANCE", "UNAVAILABLE", "OUTCOME_UNKNOWN"] as const;
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

/** Write request payload: op, value LE32, tag over those 5 bytes. */
export function writeRequest(key: Uint8Array, c: Challenge, op: Op, value: number): Uint8Array {
  if (!Number.isInteger(value) || value < 0 || value > 0xffffffff) throw new Error("Geçersiz değer.");
  const body = concat(Uint8Array.of(op), le32(value));
  return concat(body, tag(key, REQ, c, body));
}

export interface WriteResult {
  outcome: Outcome; op: number; intervalS: number; bleWindowS: number;
  serviceMode: number; services: number; revision: number;
}

/** The tasma's signed answer; throws unless the tasma itself signed it. */
export function parseResult(key: Uint8Array, c: Challenge, m: AppMessage, op: Op): WriteResult {
  if (m.type === ERROR_TYPE) throw writeRefused(m.payload[0]);
  const p = m.payload;
  if (m.type !== WRITE.writeAnswer || p.length !== 30) throw new Error("Tasmadan beklenmedik yanıt.");
  if (!equal(tag(key, RES, c, p.subarray(0, 14)), p.subarray(14, 30)))
    throw new Error("Tasmanın cevabı doğrulanamadı; ayarın yazıldığını varsayma.");
  if (p[1] !== op) throw new Error("Tasmadan beklenmedik yanıt.");
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
