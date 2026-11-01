// Account-authorized SERVER observation feed transport only. These records
// must be authenticated with the matching per-epoch traffic read key, then
// committed durably in a separate site-scoped SQLite mirror. Never treat this
// response as a phone-local Edge ACCEPT or as proof of live LoRa connection.
export interface ServerObservationRow {
  cursor: number;
  device: string;
  incarnation: string;
  sequence: number;
  family: 1 | 2;
  frame: string;
  first_seen_utc: number | null;
  rssi: number | null;
  snr: number | null;
}
export interface ServerObservationPage {
  records: ServerObservationRow[];
  cursor: number;
  has_more: boolean;
}
/** Newest MEASURED position per tracker; never moves the history cursor. */
export interface ServerLatestObservations {
  records: ServerObservationRow[];
}

const HEX16 = /^[0-9A-F]{16}$/;
const HEX_FRAME = /^[0-9A-F]+$/;
const PAGE_MAX = 64;
const LATEST_MAX = 64;

function integer(n: unknown, min: number, max: number): n is number {
  return typeof n === "number" && Number.isSafeInteger(n) && n >= min && n <= max;
}
function optionalInt(n: unknown, min: number, max: number): n is number | null {
  return n === null || integer(n, min, max);
}

function row(raw: unknown, minCursor: number): ServerObservationRow {
  if (!raw || typeof raw !== "object" || Array.isArray(raw))
    throw new Error("Sunucu kayıt öğesi geçersiz.");
  const r = raw as Record<string, unknown>;
  if (!integer(r.cursor, minCursor, Number.MAX_SAFE_INTEGER) ||
      typeof r.device !== "string" || !HEX16.test(r.device) ||
      typeof r.incarnation !== "string" || !HEX16.test(r.incarnation) ||
      !integer(r.sequence, 1, Number.MAX_SAFE_INTEGER) ||
      (r.family !== 1 && r.family !== 2) ||
      typeof r.frame !== "string" || !HEX_FRAME.test(r.frame) ||
      r.frame.length !== (r.family === 1 ? 200 : 168) ||
      !optionalInt(r.first_seen_utc, 1, 0xFFFFFFFF) ||
      !optionalInt(r.rssi, -200, 50) ||
      !optionalInt(r.snr, -50, 50))
    throw new Error("Sunucu kayıt doğrulaması başarısız.");
  return r as unknown as ServerObservationRow;
}

export function parseServerLatest(body: unknown): ServerLatestObservations {
  if (!body || typeof body !== "object" || Array.isArray(body))
    throw new Error("Sunucu son konum yanıtı hatalı.");
  const b = body as Record<string, unknown>;
  if (b.latest !== true || !Array.isArray(b.records) || b.records.length > LATEST_MAX)
    throw new Error("Sunucu son konum yanıtı geçersiz.");
  const records = b.records.map(r => row(r, 1));
  const devices = new Set(records.map(r => r.device));
  if (devices.size !== records.length || records.some(r => r.family !== 1))
    throw new Error("Sunucu son konum yanıtı tutarsız.");
  return { records };
}

export function parseServerObservationPage(body: unknown, after: number): ServerObservationPage {
  if (!integer(after, 0, Number.MAX_SAFE_INTEGER))
    throw new Error("Sunucu kayıt imleci geçersiz.");
  if (!body || typeof body !== "object" || Array.isArray(body))
    throw new Error("Sunucu kayıt yanıtı hatalı.");
  const b = body as Record<string, unknown>;
  if (!Array.isArray(b.records) || b.records.length > PAGE_MAX ||
      typeof b.has_more !== "boolean" ||
      !integer(b.cursor, after, Number.MAX_SAFE_INTEGER) ||
      (b.has_more && b.records.length === 0))
    throw new Error("Sunucu kayıt sayfası geçersiz.");

  const records: ServerObservationRow[] = [];
  let last = after;
  for (const raw of b.records) {
    const r = row(raw, last + 1);
    last = r.cursor;
    records.push(r);
  }
  if (b.cursor !== last || (records.length === 0 && b.has_more))
    throw new Error("Sunucu kayıt imleci uyuşmuyor.");
  return { cursor: last, has_more: b.has_more, records };
}
