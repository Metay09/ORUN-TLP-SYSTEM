// Gateway <-> Edge link, transport neutral. Mirrors firmware
// include/edge_link.h (BLE, SF5L6) and the USB text lines of SF5L1/SF5L5.
// Shared vectors: test/core.test.ts.
import { ascii, be16, be64, concat, hex, hex64, i16, i8, padTo, u16, u64 } from "./bytes.ts";
import { hkdfSha256, hmacSha256, sha256 } from "./crypto.ts";
import { TEST_CREDENTIAL_ID, TEST_ROOT } from "./product.ts";

export const EDGE_SERVICE_UUID = "6f9a1c20-5b3e-4d8a-9e61-2c4b7a0d3e51";
export const EDGE_TO_GATEWAY_UUID = "6f9a1c21-5b3e-4d8a-9e61-2c4b7a0d3e51";
export const EDGE_TO_EDGE_UUID = "6f9a1c22-5b3e-4d8a-9e61-2c4b7a0d3e51";

export const FRAME_SIZE = 20;
export const FRAME_DATA = 16;
export const MAX_MESSAGE = 160;
export const REASSEMBLY_TIMEOUT_MS = 3000;

/** Splits one logical message into 20-byte frames (Fragmenter). */
export function frames(message: Uint8Array, messageId: number): Uint8Array[] {
  if (message.length === 0 || message.length > MAX_MESSAGE) throw new Error("message size");
  const out: Uint8Array[] = [];
  for (let offset = 0, index = 0; offset < message.length; index++) {
    const chunk = message.subarray(offset, offset + FRAME_DATA);
    const flags = (index === 0 ? 1 : 0) | (offset + chunk.length === message.length ? 2 : 0);
    out.push(concat(new Uint8Array([1, flags, messageId & 0xff, index]), chunk));
    offset += chunk.length;
  }
  return out;
}

/** Rebuilds one message; any gap/reorder/version/size problem drops it. */
export class Reassembler {
  private active = false;
  private buf: number[] = [];
  private id = 0;
  private next = 0;
  private started = 0;

  reset(): void { this.active = false; this.buf = []; }

  push(frame: Uint8Array, nowMs = 0): Uint8Array | null {
    if (frame.length <= 4 || frame.length > FRAME_SIZE || frame[0] !== 1 || (frame[1] & ~3)) {
      this.reset();
      return null;
    }
    const start = (frame[1] & 1) !== 0, end = (frame[1] & 2) !== 0;
    if (start) {
      if (frame[3] !== 0) { this.reset(); return null; }
      this.active = true; this.buf = []; this.id = frame[2]; this.next = 0;
      this.started = nowMs;
    } else if (!this.active || frame[2] !== this.id || frame[3] !== this.next) {
      this.reset();
      return null;
    }
    if (this.buf.length + frame.length - 4 > MAX_MESSAGE) { this.reset(); return null; }
    for (const v of frame.subarray(4)) this.buf.push(v);
    this.next++;
    if (!end) return null;
    this.active = false;
    return new Uint8Array(this.buf);
  }

  poll(nowMs: number): void {
    if (this.active && nowMs - this.started >= REASSEMBLY_TIMEOUT_MS) this.reset();
  }
}

// ---- Messages ------------------------------------------------------------
export type GatewayMessage =
  | { type: "hello"; gateway: bigint; held: number; capacity: number }
  | { type: "held" | "heldPreview"; page: number; slot: number; generation: bigint;
      link: { rssi: number; snr: number } | null; object: Uint8Array }
  | { type: "dumpEnd"; count: number }
  | { type: "acceptResult"; page: number; slot: number; code: number }
  | { type: "reclaimed"; page: number; ok: boolean };

export function decodeGatewayMessage(m: Uint8Array): GatewayMessage | null {
  if (!m.length) return null;
  switch (m[0]) {
    case 0x01:
      return m.length === 13
        ? { type: "hello", gateway: u64(m, 1), held: u16(m, 9), capacity: u16(m, 11) }
        : null;
    case 0x02:
    case 0x06: // Optional newest custody preview, excluded from DUMP_END
      if (m.length < 18 || m.length !== 18 + m[17]) return null;
      return {
        type: m[0] === 0x06 ? "heldPreview" : "held",
        page: u16(m, 1), slot: u16(m, 3), generation: u64(m, 5),
        link: m[13] & 1 ? { rssi: i16(m, 14), snr: i8(m, 16) } : null,
        object: m.slice(18),
      };
    case 0x03:
      return m.length === 3 ? { type: "dumpEnd", count: u16(m, 1) } : null;
    case 0x04:
      return m.length === 6
        ? { type: "acceptResult", page: u16(m, 1), slot: u16(m, 3), code: m[5] }
        : null;
    case 0x05:
      return m.length === 4 ? { type: "reclaimed", page: u16(m, 1), ok: m[3] !== 0 } : null;
    default:
      return null;
  }
}

/** ACCEPT_RESULT code -> the USB reply word (OK covers handed off + already). */
export const ACCEPT_WORDS: Record<number, string> = {
  0: "OK", 1: "OK", 2: "UNKNOWN", 3: "BUSY", 4: "REJECTED", 5: "REJECTED",
  6: "REJECTED", 7: "FAIL",
};

export const DUMP_MESSAGE = new Uint8Array([0x81]);

// ---- Edge accept (SF5L5) -------------------------------------------------
// HMAC-SHA256(K_edge, M), M = "ORUN-EDGE-ACCEPT-v1" | gw | page | slot | gen |
// SHA-256(object), zero padded to 102 bytes. K_edge = HKDF(salt = test
// credential id, ikm = test root, info = "ORUN-SF5L0-BENCH-EDGE-ACCEPT-v1" |
// gw, zero padded to 57 bytes). BENCH ONLY: public test key.
export function acceptMac(gateway: bigint, page: number, slot: number,
                          generation: bigint, object: Uint8Array): Uint8Array {
  const info = padTo(concat(ascii("ORUN-SF5L0-BENCH-EDGE-ACCEPT-v1"), be64(gateway)), 57);
  const key = hkdfSha256(TEST_ROOT, TEST_CREDENTIAL_ID, info, 32);
  const msg = padTo(concat(ascii("ORUN-EDGE-ACCEPT-v1"), be64(gateway), be16(page),
                           be16(slot), be64(generation), sha256(object)), 102);
  return hmacSha256(key, msg);
}

/** BLE ACCEPT: type(1) page(2) slot(2) gen(8) mac(32). */
export function acceptMessage(gateway: bigint, page: number, slot: number,
                              generation: bigint, object: Uint8Array): Uint8Array {
  return concat(new Uint8Array([0x82]), be16(page), be16(slot), be64(generation),
                acceptMac(gateway, page, slot, generation, object));
}

/** USB: EDGE ACCEPT v1 page=P slot=S gen=<16 hex> mac=<64 hex>. */
export function acceptLine(gateway: bigint, page: number, slot: number,
                           generation: bigint, object: Uint8Array): string {
  return `EDGE ACCEPT v1 page=${page} slot=${slot} gen=${hex64(generation)} ` +
    `mac=${hex(acceptMac(gateway, page, slot, generation, object))}`;
}

// ---- USB text lines ------------------------------------------------------
const HELD_LINE = /^EDGE (HELD|PRIORITY) v1 gw=([0-9A-F]{16}) page=(\d+) slot=(\d+) gen=([0-9A-F]{16}) len=(\d+) obj=([0-9A-F]+)$/;
const RX_LINE = /RX PRODUCT_SECURE queued family=\d+ length=\d+ rssi=(-?\d+) snr=(-?\d+)/;
// The Gateway prints "EDGE ACCEPT REJECTED reason=MAC page=P slot=S".
const ACCEPT_REPLY = /EDGE ACCEPT (OK|UNKNOWN|BUSY|REJECTED|FAIL)(?: reason=\w+)? page=(\d+|\?) slot=(\d+|\?)/;
const RECLAIM = /EDGE RECLAIM page=(\d+) (started|OK|FAIL)/;
const CUSTODY_STARTED = "BENCH GW RX record; durable custody write started";
const RX_NOT_NEW = ["BENCH GW RX duplicate already held", "BENCH GW RX denied"];

export type UsbEvent =
  | { type: "held" | "heldPreview"; gateway: bigint; page: number; slot: number; generation: bigint;
      link: { rssi: number; snr: number } | null; object: Uint8Array }
  | { type: "heldBroken"; gateway: string; page: number; slot: number; got: number; want: number }
  | { type: "acceptReply"; word: string; page: number | null; slot: number | null }
  | { type: "reclaim"; page: number; what: "started" | "OK" | "FAIL" }
  | { type: "dumpEnd"; text: string };

/**
 * Turns Gateway serial lines into events. The RSSI/SNR of the Gateway's RX
 * line belongs to the record only if the next lines are "custody write
 * started" and then its EDGE HELD; anything else drops it (SF5L4).
 */
export class UsbLineParser {
  private rx: { rssi: number; snr: number } | null = null;
  private armed: { rssi: number; snr: number } | null = null;

  line(text: string): UsbEvent | null {
    let m = ACCEPT_REPLY.exec(text);
    if (m) {
      const known = m[2] !== "?";
      return { type: "acceptReply", word: m[1], page: known ? Number(m[2]) : null,
               slot: known ? Number(m[3]) : null };
    }
    m = RECLAIM.exec(text);
    if (m) return { type: "reclaim", page: Number(m[1]), what: m[2] as "started" | "OK" | "FAIL" };
    m = RX_LINE.exec(text);
    if (m) { this.rx = { rssi: Number(m[1]), snr: Number(m[2]) }; this.armed = null; return null; }
    if (text.includes(CUSTODY_STARTED)) { this.armed = this.rx; this.rx = null; return null; }
    if (RX_NOT_NEW.some((t) => text.includes(t))) { this.rx = this.armed = null; return null; }
    if (text.startsWith("EDGE DUMP END")) return { type: "dumpEnd", text };
    m = HELD_LINE.exec(text);
    if (!m) return null;
    const link = m[1] === "HELD" ? this.armed : null;
    this.armed = this.rx = null;
    const obj = m[7], want = 2 * Number(m[6]);
    if (obj.length % 2 || obj.length !== want)
      return { type: "heldBroken", gateway: m[2], page: Number(m[3]), slot: Number(m[4]),
               got: obj.length, want };
    const bytes = new Uint8Array(obj.length / 2);
    for (let i = 0; i < bytes.length; i++) bytes[i] = parseInt(obj.slice(2 * i, 2 * i + 2), 16);
    return { type: m[1] === "PRIORITY" ? "heldPreview" : "held",
             gateway: BigInt("0x" + m[2]), page: Number(m[3]), slot: Number(m[4]),
             generation: BigInt("0x" + m[5]), link, object: bytes };
  }
}
