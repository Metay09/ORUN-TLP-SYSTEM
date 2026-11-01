// 4a: ORUN uygulama GATT servisi (tasma ve gateway, M7P7F/M7P7H). 20 baytlık
// çerçeveler: 8 bayt başlık + en çok 12 bayt veri; mantıksal mesaj en çok 48
// bayt, 4 parça. Sayılar küçük-uçlu (little-endian). Bir istek, bir yanıt.
export const APP_SERVICE_UUID = "0088f2c3-cc13-4b5d-b025-f7fcb7c71af2";
export const APP_REQUEST_UUID = "fbf521f6-ce25-4e6a-af6c-c76303803ee3";
export const APP_RESPONSE_UUID = "c0ba4044-1242-447b-9f79-e97aa0065ad3";

const VERSION = 0x01, HEADER = 8, MAX_FRAME = 20, MAX_PAYLOAD = 48, START = 0x01, END = 0x02;

/** Request types; the response type is request | 0x80, an error is 0xFF. */
export const QUERY = {
  config: 0x01, device: 0x02, tracking: 0x03, geofence: 0x04, storage: 0x05, power: 0x06, services: 0x07,
} as const;
export type Family = keyof typeof QUERY;
export const ERROR_TYPE = 0xff;
const ERROR_TEXT: Record<number, string> = {
  1: "desteklenmiyor", 2: "meşgul", 3: "izin yok", 4: "şu an hazır değil",
};

/** One request frame (all queries carry no data). */
export function requestFrame(type: number, correlation: number): Uint8Array {
  const f = new Uint8Array(HEADER);
  f[0] = VERSION; f[1] = type; f[2] = START | END; f[3] = 0;
  f[4] = correlation & 0xff; f[5] = (correlation >> 8) & 0xff;
  return f;
}

export interface AppMessage { type: number; correlation: number; payload: Uint8Array; }

/** Collects response fragments; `push` returns a whole message once complete. */
export class AppReassembler {
  private cur: { type: number; correlation: number; total: number; next: number; data: number[] } | null = null;

  reset(): void { this.cur = null; }

  push(frame: Uint8Array): AppMessage | null {
    if (frame.length < HEADER || frame.length > MAX_FRAME || frame[0] !== VERSION) { this.cur = null; return null; }
    const type = frame[1], flags = frame[2], index = frame[3];
    const correlation = frame[4] | (frame[5] << 8);
    const total = frame[6] | (frame[7] << 8);
    if (total > MAX_PAYLOAD || (flags & ~(START | END))) { this.cur = null; return null; }
    if (flags & START) {
      if (index !== 0) { this.cur = null; return null; }
      this.cur = { type, correlation, total, next: 0, data: [] };
    }
    const c = this.cur;
    if (!c || c.type !== type || c.correlation !== correlation || c.total !== total || c.next !== index) {
      this.cur = null; return null;
    }
    c.data.push(...frame.subarray(HEADER));
    c.next += 1;
    if (c.data.length > c.total) { this.cur = null; return null; }
    if (!(flags & END)) return null;
    this.cur = null;
    if (c.data.length !== c.total) return null;
    return { type, correlation, payload: Uint8Array.from(c.data) };
  }
}

/** The answer to `family`, or an error the device gave. */
export function checkAnswer(m: AppMessage, family: Family): Uint8Array {
  if (m.type === ERROR_TYPE) throw new Error(`Cihaz ${family} sorusunu yanıtlamadı: ${ERROR_TEXT[m.payload[0]] ?? "hata"}.`);
  if (m.type !== (QUERY[family] | 0x80)) throw new Error("Cihazdan beklenmedik yanıt.");
  if (m.payload[0] !== 0) throw new Error("Cihaz hata bildirdi.");
  return m.payload;
}
